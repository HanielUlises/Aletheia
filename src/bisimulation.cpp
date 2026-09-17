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
//   round k:  key(w) = ( class_{k-1}(w), ⟨rank of sorted class_{k-1} of R_i(w)⟩_{i∈Ag} )
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
struct Scratch {
    std::vector<bits::Word>    reach;
    std::vector<std::uint8_t>  set_seen;
    std::vector<WorldIdx>      frontier;
    std::vector<std::int32_t>  class_of, next_class;
    std::vector<WorldIdx>      order;
    std::vector<std::int32_t>  key_data;
    std::vector<std::uint32_t> key_begin;
    std::vector<std::int32_t>  nset_data;    // N(S): sorted classes of S's members
    std::vector<std::uint32_t> nset_begin;
    std::vector<std::uint32_t> set_order;
    std::vector<std::int32_t>  rank_of_set;
    std::vector<std::int32_t>  keys;
    std::vector<std::uint32_t> stamp;
    std::vector<WorldIdx>      repr;
};

Scratch& scratch() {
    thread_local Scratch sc;
    return sc;
}

thread_local std::uint32_t t_rounds = 0;

// Drops worlds unreachable from W*. Each successor set is expanded once;
// returns `s` without copying when every world is reachable.
EpistemicState restrict_to_reachable(EpistemicState s) {
    const std::uint32_t nw = s.num_worlds;

    auto& sc    = scratch();
    auto& reach = sc.reach;
    reach.assign(s.rel_words, 0);
    bits::copy_from(reach, s.designated_bits());
    sc.set_seen.assign(s.num_sets(), 0);

    auto& frontier = sc.frontier;
    frontier.clear();
    bits::for_each(s.designated_bits(),
                   [&](std::uint32_t w) { frontier.push_back(w); });

    std::size_t reached = frontier.size();
    while (!frontier.empty()) {
        const WorldIdx w = frontier.back();
        frontier.pop_back();
        for (AgentIdx ag = 0; ag < s.num_agents; ++ag) {
            const std::uint32_t id = s.succ_set(ag, w);
            if (sc.set_seen[id]) continue;
            sc.set_seen[id] = 1;
            for (WorldIdx v : s.set(id)) {
                if (bits::test(reach, v)) continue;
                bits::set(reach, v);
                frontier.push_back(v);
                ++reached;
            }
        }
    }

    if (reached == nw) return s;

    std::vector<WorldIdx> remap;
    return restrict_state(s, reach, remap);
}

// N(S) for every set: the sorted, duplicate-free classes of its members.
void neighbour_classes(const EpistemicState& m, const std::vector<std::int32_t>& class_of,
                       Scratch& sc) {
    const std::uint32_t ns = m.num_sets();
    auto& data  = sc.nset_data;
    auto& begin = sc.nset_begin;
    auto& stamp = sc.stamp;
    data.clear();
    begin.assign(std::size_t(ns) + 1, 0);
    if (stamp.size() < m.num_worlds) stamp.assign(m.num_worlds, 0);

    std::uint32_t token = 0;
    for (std::uint32_t id = 0; id < ns; ++id) {
        begin[id] = static_cast<std::uint32_t>(data.size());
        if (++token == 0) {
            std::fill(stamp.begin(), stamp.end(), 0);
            token = 1;
        }
        for (WorldIdx v : m.set(id)) {
            const std::int32_t c = class_of[v];
            if (stamp[c] != token) {
                stamp[c] = token;
                data.push_back(c);
            }
        }
        std::sort(data.begin() + begin[id], data.end());
    }
    begin[ns] = static_cast<std::uint32_t>(data.size());
    // Stamps are per call; leave them zeroed for the next one.
    std::fill(stamp.begin(), stamp.end(), 0);
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
    // key(w) = (class(w), rank(N(R_1(w))), …), N(S) = { class(v) | v ∈ S },
    // rank ordering distinct N by (size, contents). Ranks are taken over all
    // sets rather than per agent: the order they induce on each agent's sets
    // is the same, so the numbering is unchanged.
    const std::uint32_t ns        = m.num_sets();
    const std::size_t   key_width = std::size_t(na) + 1;
    auto& keys = sc.keys;
    keys.resize(std::size_t(nw) * key_width);

    auto& set_order   = sc.set_order;
    auto& rank_of_set = sc.rank_of_set;

    std::int32_t num_classes = *std::max_element(class_of.begin(), class_of.end()) + 1;

    t_rounds = 0;
    for (;;) {
        ++t_rounds;
        neighbour_classes(m, class_of, sc);
        const auto nset_at = [&](std::uint32_t id) {
            return std::span<const std::int32_t>(sc.nset_data.data() + sc.nset_begin[id],
                                                 sc.nset_begin[id + 1] - sc.nset_begin[id]);
        };

        set_order.resize(ns);
        for (std::uint32_t id = 0; id < ns; ++id) set_order[id] = id;
        std::sort(set_order.begin(), set_order.end(), [&](std::uint32_t a, std::uint32_t b) {
            const auto sa = nset_at(a), sb = nset_at(b);
            if (sa.size() != sb.size()) return sa.size() < sb.size();
            return std::lexicographical_compare(sa.begin(), sa.end(), sb.begin(), sb.end());
        });
        rank_of_set.resize(ns);
        std::int32_t rank = 0;
        rank_of_set[set_order[0]] = 0;
        for (std::uint32_t i = 1; i < ns; ++i) {
            const auto sa = nset_at(set_order[i - 1]), sb = nset_at(set_order[i]);
            if (!std::equal(sa.begin(), sa.end(), sb.begin(), sb.end())) ++rank;
            rank_of_set[set_order[i]] = rank;
        }

        for (WorldIdx w = 0; w < nw; ++w) {
            const std::size_t k = std::size_t(w) * key_width;
            keys[k] = class_of[w];
            for (AgentIdx ag = 0; ag < na; ++ag)
                keys[k + 1 + ag] = rank_of_set[m.succ_set(ag, w)];
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
    // Class ids are already canonical, so world c of the result is class c. The
    // last round's N(S) were computed from the final classes; interning them by
    // content in (agent, class) order numbers the sets canonically too.
    auto& repr = sc.repr;
    repr.assign(num_classes, kNoWorld);
    for (WorldIdx w = 0; w < nw; ++w)
        if (repr[class_of[w]] == kNoWorld) repr[class_of[w]] = w;

    EpistemicState out;
    out.allocate(static_cast<std::uint32_t>(num_classes), m.num_atoms, na);

    for (std::int32_t c = 0; c < num_classes; ++c)
        bits::copy_from(out.val(static_cast<WorldIdx>(c)), m.val(repr[c]));

    {
        SetInterner interner(out);
        thread_local std::vector<WorldIdx> buf;
        std::vector<std::uint32_t> out_id(ns, UINT32_MAX);
        const auto nc = static_cast<std::uint32_t>(num_classes);
        for (AgentIdx ag = 0; ag < na; ++ag)
            for (std::uint32_t c = 0; c < nc; ++c) {
                const std::uint32_t id = m.succ_set(ag, repr[c]);
                if (out_id[id] == UINT32_MAX) {
                    buf.assign(sc.nset_data.begin() + sc.nset_begin[id],
                               sc.nset_data.begin() + sc.nset_begin[id + 1]);
                    out_id[id] = interner.intern(buf);
                }
                out.set_of[std::size_t(ag) * nc + c] = out_id[id];
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

std::uint32_t last_refinement_rounds() noexcept { return t_rounds; }
