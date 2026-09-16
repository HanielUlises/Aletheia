#include "bisimulation.hpp"

#include <algorithm>
#include <bit>
#include <span>
#include <vector>


// Contraction = reachability restriction + ordered partition refinement.
//
// Two worlds w, v of a multi-pointed model are bisimilar when
//
//   (i)   V(w) = V(v),
//   (ii)  w ∈ W* ⇔ v ∈ W*,
//   (iii) for every agent i, every R_i-successor of w has a bisimilar
//         R_i-successor of v, and symmetrically.
//
// Condition (ii) is what makes the quotient a pointed invariant. Omitting it
// still preserves formula truth, since bisimilar worlds satisfy the same
// formulas, but it permits a designated world to merge with a non-designated
// one. The quotient then no longer determines W*, and the canonical form below
// would identify states that are distinct planning situations. Retaining it
// costs at most a coarser contraction and yields a sound structural identity.
//
// Why ordered refinement rather than Paige–Tarjan.
//
// Paige-Tarjan refines in O(m log n), asymptotically better than the O(r·(m +
// n log n)) loop below (r = number of rounds, bounded by n and in practice
// small). It does not, however, produce a canonical numbering of the resulting
// classes, and the planner requires one: the closed list identifies states by
// fingerprint, so bisimilar models must serialise identically.
//
// This implementation obtains canonicity from the refinement itself. Each round
// sorts worlds by a key and assigns class ids in sorted order:
//
//   round 0:  key(w) = ( [w ∈ W*], V(w) )
//   round k:  key(w) = ( class_{k-1}(w), ⟨sorted class_{k-1} of R_i(w)⟩_{i∈Ag} )
//
// Round 0's key is a function of the model alone. By induction each later key
// is too, so the class ids at the fixpoint are determined by the isomorphism
// class of the model and nothing else. Renumbering worlds by final class id is
// then a canonical form.
//
// The fixpoint test is exact: since the round-k key begins with the round-(k-1)
// class, sorting is order-preserving on the previous partition, so ids are
// stable and "no class split" is exactly "assignment unchanged".


namespace {

// Thread-local working buffers: no allocation per call beyond the result.
struct RowGroups {
    std::vector<std::uint32_t> group_of;   // world → group
    std::vector<WorldIdx>      rep;        // group → representative world
};

struct Scratch {
    std::vector<bits::Word>    reach;
    std::vector<WorldIdx>      frontier;
    std::vector<std::int32_t>  class_of, next_class;
    std::vector<WorldIdx>      order;
    std::vector<std::int32_t>  key_data;
    std::vector<std::uint32_t> key_begin;
    std::vector<RowGroups>     groups;
    std::vector<std::int32_t>  keys;
    std::vector<std::int32_t>  set_data;
    std::vector<std::uint32_t> set_begin;
    std::vector<std::uint32_t> group_order;
    std::vector<std::int32_t>  rank_of_group;
    std::vector<std::uint32_t> stamp;
    std::vector<WorldIdx>      repr;
};

Scratch& scratch() {
    thread_local Scratch sc;
    return sc;
}

// Worlds unreachable from W* cannot affect the truth of any formula evaluated
// at a designated world, so they are dropped before refinement rather than
// carried through it. The product update materialises the full W × E cross
// product, which regularly leaves such worlds behind.
// Word-parallel BFS; returns `s` without copying when all worlds are reachable.
EpistemicState restrict_to_reachable(EpistemicState s) {
    const std::uint32_t nw = s.num_worlds;
    const std::uint32_t rw = s.rel_words;

    auto& reach = scratch().reach;
    reach.assign(rw, 0);
    bits::copy_from(reach, s.designated_bits());

    auto& frontier = scratch().frontier;
    frontier.clear();
    bits::for_each(s.designated_bits(),
                   [&](std::uint32_t w) { frontier.push_back(w); });

    while (!frontier.empty()) {
        const WorldIdx w = frontier.back();
        frontier.pop_back();
        for (AgentIdx ag = 0; ag < s.num_agents; ++ag) {
            const auto row = s.succ(ag, w);
            for (std::uint32_t i = 0; i < rw; ++i) {
                bits::Word fresh = row[i] & ~reach[i];
                if (!fresh) continue;
                reach[i] |= fresh;
                while (fresh) {
                    frontier.push_back(static_cast<WorldIdx>(
                        i * bits::kWordBits + std::countr_zero(fresh)));
                    fresh &= fresh - 1;
                }
            }
        }
    }

    if (bits::count(reach) == nw) return s;

    std::vector<WorldIdx> remap;
    return restrict_state(s, reach, remap);
}

// Groups worlds by identical R_i row, so N_i is computed once per row (once
// per class on S5). Group ids are not part of the canonical form.
void group_rows(const EpistemicState& m, AgentIdx ag,
                std::vector<WorldIdx>& order, RowGroups& g) {
    const std::uint32_t nw = m.num_worlds;
    g.group_of.resize(nw);
    g.rep.clear();

    order.resize(nw);
    for (WorldIdx w = 0; w < nw; ++w) order[w] = w;
    std::sort(order.begin(), order.end(), [&](WorldIdx x, WorldIdx y) {
        const auto rx = m.succ(ag, x), ry = m.succ(ag, y);
        return std::lexicographical_compare(rx.begin(), rx.end(), ry.begin(), ry.end());
    });

    g.rep.push_back(order[0]);
    g.group_of[order[0]] = 0;
    for (std::uint32_t i = 1; i < nw; ++i) {
        if (!bits::equal(m.succ(ag, order[i - 1]), m.succ(ag, order[i])))
            g.rep.push_back(order[i]);
        g.group_of[order[i]] = static_cast<std::uint32_t>(g.rep.size() - 1);
    }
}

} // namespace

EpistemicState bisim_contract(EpistemicState s) {
    if (s.num_worlds == 0) return s;

    EpistemicState m  = restrict_to_reachable(std::move(s));
    const std::uint32_t nw = m.num_worlds;
    const std::uint32_t na = m.num_agents;
    if (nw == 0) return m;

    Scratch& sc = scratch();
    auto& class_of   = sc.class_of;
    auto& next_class = sc.next_class;
    auto& order      = sc.order;
    class_of.assign(nw, 0);
    next_class.assign(nw, 0);
    order.resize(nw);

    //  Round 0: valuation and designation.
    {
        auto& key_data  = sc.key_data;
        auto& key_begin = sc.key_begin;
        key_data.clear();
        key_begin.assign(nw + 1, 0);
        for (WorldIdx w = 0; w < nw; ++w) {
            key_begin[w] = static_cast<std::uint32_t>(key_data.size());
            key_data.push_back(m.is_designated(w) ? 1 : 0);
            // Valuation words, halved into int32 so the whole key is one type.
            for (bits::Word word : m.val(w)) {
                key_data.push_back(static_cast<std::int32_t>(word & 0xFFFFFFFFu));
                key_data.push_back(static_cast<std::int32_t>(word >> 32));
            }
        }
        key_begin[nw] = static_cast<std::uint32_t>(key_data.size());

        const auto key_at = [&](WorldIdx w) {
            return std::span<const std::int32_t>(key_data.data() + key_begin[w],
                                                 key_begin[w + 1] - key_begin[w]);
        };

        for (WorldIdx w = 0; w < nw; ++w) order[w] = w;
        std::sort(order.begin(), order.end(), [&](WorldIdx a, WorldIdx b) {
            const auto ka = key_at(a), kb = key_at(b);
            return std::lexicographical_compare(ka.begin(), ka.end(),
                                                kb.begin(), kb.end());
        });

        std::int32_t id = 0;
        class_of[order[0]] = 0;
        for (std::size_t i = 1; i < nw; ++i) {
            const auto ka = key_at(order[i - 1]), kb = key_at(order[i]);
            if (!std::equal(ka.begin(), ka.end(), kb.begin(), kb.end())) ++id;
            class_of[order[i]] = id;
        }
    }

    //  Rounds 1..: split on neighbour classes.
    //
    // key(w) = (class(w), rank_1(N_1(w)), …), N_i(w) = { class(v) | v ∈ R_i(w) },
    // rank_i ordering distinct N_i by (size, contents). This is the order of the
    // length-prefixed key it replaces, so the numbering is unchanged.
    auto& groups = sc.groups;
    if (groups.size() < na) groups.resize(na);
    for (AgentIdx ag = 0; ag < na; ++ag) group_rows(m, ag, order, groups[ag]);

    const std::size_t key_width = std::size_t(na) + 1;
    auto& keys = sc.keys;
    keys.resize(std::size_t(nw) * key_width);

    auto& set_data      = sc.set_data;     // concatenated N_i per group
    auto& set_begin     = sc.set_begin;
    auto& group_order   = sc.group_order;
    auto& rank_of_group = sc.rank_of_group;
    auto& stamp         = sc.stamp;
    stamp.assign(nw, 0);
    std::uint32_t stamp_token = 0;

    std::int32_t num_classes = *std::max_element(class_of.begin(), class_of.end()) + 1;

    for (;;) {
        for (WorldIdx w = 0; w < nw; ++w)
            keys[std::size_t(w) * key_width] = class_of[w];

        for (AgentIdx ag = 0; ag < na; ++ag) {
            const RowGroups& g  = groups[ag];
            const auto       ng = static_cast<std::uint32_t>(g.rep.size());

            set_data.clear();
            set_begin.assign(ng + 1, 0);
            for (std::uint32_t gi = 0; gi < ng; ++gi) {
                set_begin[gi] = static_cast<std::uint32_t>(set_data.size());
                ++stamp_token;
                bits::for_each(m.succ(ag, g.rep[gi]), [&](std::uint32_t v) {
                    const std::int32_t c = class_of[v];
                    if (stamp[c] != stamp_token) {
                        stamp[c] = stamp_token;
                        set_data.push_back(c);
                    }
                });
                std::sort(set_data.begin() + set_begin[gi], set_data.end());
            }
            set_begin[ng] = static_cast<std::uint32_t>(set_data.size());

            const auto set_at = [&](std::uint32_t gi) {
                return std::span<const std::int32_t>(set_data.data() + set_begin[gi],
                                                     set_begin[gi + 1] - set_begin[gi]);
            };

            group_order.resize(ng);
            for (std::uint32_t gi = 0; gi < ng; ++gi) group_order[gi] = gi;
            std::sort(group_order.begin(), group_order.end(),
                      [&](std::uint32_t a, std::uint32_t b) {
                          const auto sa = set_at(a), sb = set_at(b);
                          if (sa.size() != sb.size()) return sa.size() < sb.size();
                          return std::lexicographical_compare(sa.begin(), sa.end(),
                                                              sb.begin(), sb.end());
                      });

            rank_of_group.resize(ng);
            std::int32_t rank = 0;
            rank_of_group[group_order[0]] = 0;
            for (std::uint32_t i = 1; i < ng; ++i) {
                const auto sa = set_at(group_order[i - 1]), sb = set_at(group_order[i]);
                if (!std::equal(sa.begin(), sa.end(), sb.begin(), sb.end())) ++rank;
                rank_of_group[group_order[i]] = rank;
            }

            for (WorldIdx w = 0; w < nw; ++w)
                keys[std::size_t(w) * key_width + 1 + ag] = rank_of_group[g.group_of[w]];
        }

        const auto key_at = [&](WorldIdx w) {
            return keys.data() + std::size_t(w) * key_width;
        };

        for (WorldIdx w = 0; w < nw; ++w) order[w] = w;
        std::sort(order.begin(), order.end(), [&](WorldIdx a, WorldIdx b) {
            return std::lexicographical_compare(key_at(a), key_at(a) + key_width,
                                                key_at(b), key_at(b) + key_width);
        });

        std::int32_t id = 0;
        next_class[order[0]] = 0;
        for (std::size_t i = 1; i < nw; ++i) {
            if (!std::equal(key_at(order[i - 1]), key_at(order[i - 1]) + key_width,
                            key_at(order[i])))
                ++id;
            next_class[order[i]] = id;
        }

        // Refinement only splits: same class count ⇒ same assignment.
        if (id + 1 == num_classes) break;

        class_of.swap(next_class);
        num_classes = id + 1;
    }

    // Quotient.
    //
    // Class ids are already canonical, so world c of the result is class c.
    auto& repr = sc.repr;
    repr.assign(num_classes, kNoWorld);
    for (WorldIdx w = 0; w < nw; ++w)
        if (repr[class_of[w]] == kNoWorld) repr[class_of[w]] = w;

    EpistemicState out;
    out.allocate(static_cast<std::uint32_t>(num_classes), m.num_atoms, na);

    for (std::int32_t c = 0; c < num_classes; ++c)
        bits::copy_from(out.val(static_cast<WorldIdx>(c)), m.val(repr[c]));

    for (AgentIdx ag = 0; ag < na; ++ag) {
        for (std::int32_t c = 0; c < num_classes; ++c) {
            auto dst = out.succ(ag, static_cast<WorldIdx>(c));
            bits::for_each(m.succ(ag, repr[c]), [&](std::uint32_t v) {
                bits::set(dst, static_cast<WorldIdx>(class_of[v]));
            });
        }
    }

    // Designation is constant within a class by construction (round 0 split on
    // it), so testing the representative is exact.
    auto des = out.designated_bits();
    for (std::int32_t c = 0; c < num_classes; ++c)
        if (m.is_designated(repr[c])) bits::set(des, static_cast<WorldIdx>(c));

    out.invalidate();
    return out;
}
