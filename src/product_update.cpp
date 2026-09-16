#include "product_update.hpp"

#include <algorithm>
#include <bit>
#include <cassert>
#include <vector>

// DEL product update  M ⊗ A
//
// Given M = (W, {R_i}, V, W*) and an event model A = (E, {R^E_i}, pre, post, E_d):
//
//   W'          = { (w,e) | w ∈ W, e ∈ E, M,w ⊨ pre(e) }
//   R'_i        = { ((w,e),(v,f)) | (w,v) ∈ R_i ∧ (e,f) ∈ R^E_i }
//   V'(w,e)(p)  = post(e)(p) if p ∈ dom(post(e)), else V(w)(p)
//   W'*         = { (w,e) | w ∈ W*, e ∈ E_d }
//
// Postconditions are conditional: p becomes true at (w,e) iff post_true[p] holds
// at w *in the pre-update model* (symmetrically for post_false). Atoms in
// neither map are inherited.
//
// R^E_i is not fixed: each agent carries an ordered list of (condition, event
// relation) cases, and the first case whose condition holds at w supplies that
// agent's event relation there. If no case matches the agent is treated as
// fully observing, R^E_i(e) = E, which over-approximates uncertainty and is
// therefore safe.
//
// Evaluation strategy. Preconditions, postcondition guards and observability
// conditions are computed once each as extensions over the whole source model,
// so that every per-pair test performed during the construction is a bit
// lookup. An observability condition is a function of w alone and independent
// of e, which is what permits hoisting its evaluation out of the pair loop.
//
// Worlds are numbered in one block per event, idx(w,e) = off(e) + rank of w in
// sat(pre(e)), so a row of R'_i is an OR of PEXT(R_i(w), sat(pre(f))) shifted to
// off(f). bisim_contract renumbers canonically afterwards.

namespace {

// Extensions the update tests, in flat thread-local buffers.
struct Scratch {
    std::uint32_t rel_words{0};

    std::vector<bits::Word>    pre;          // [e · rel_words]  sat(pre(e))
    std::vector<std::uint32_t> pre_count;    // [e]              |sat(pre(e))|
    std::vector<std::uint32_t> offset;       // [e]              off(e)

    struct Post { EventIdx event; AtomIdx atom; bool value; std::uint32_t ext; };
    std::vector<Post>          posts;        // grouped by event, true before false
    std::vector<std::uint32_t> post_begin;   // [e + 1] → range into posts
    std::vector<bits::Word>    post_ext;     // [k · rel_words]

    std::vector<bits::Word>    obs_ext;      // [c · rel_words]  one per case
    std::vector<bits::Word>    obs_claimed;  // worlds matched by an earlier case
    std::vector<std::int32_t>  obs_case;     // [ag · |W|]  first matching case, or -1

    std::vector<bits::Word>    packed;       // [f · rel_words]  PEXT(row, pre(f))
    std::vector<std::uint32_t> packed_bits;  // [f]
    std::vector<bits::Word>    all_events;   // row with every event set

    [[nodiscard]] bits::ConstWordSpan pre_of(EventIdx e) const noexcept
        { return {pre.data() + std::size_t(e) * rel_words, rel_words}; }
};

Scratch& scratch() {
    thread_local Scratch sc;
    return sc;
}

void precompute(const EpistemicState& s, const Action& a, Scratch& p) {
    const std::uint32_t ne = static_cast<std::uint32_t>(a.events.size());
    const std::uint32_t nw = s.num_worlds;
    const std::uint32_t na = s.num_agents;
    const std::uint32_t rw = s.rel_words;
    p.rel_words = rw;

    // A sat() span is invalidated by the next sat() call: copy before the next.
    p.pre.resize(std::size_t(ne) * rw);
    p.pre_count.resize(ne);
    p.offset.resize(ne);
    std::uint32_t total = 0;
    for (EventIdx e = 0; e < ne; ++e) {
        const auto ext = s.sat(*a.events[e].precondition);
        std::copy(ext.begin(), ext.end(), p.pre.begin() + std::size_t(e) * rw);
        p.pre_count[e] = static_cast<std::uint32_t>(bits::count(ext));
        p.offset[e]    = total;
        total         += p.pre_count[e];
    }

    p.posts.clear();
    p.post_begin.assign(ne + 1, 0);
    p.post_ext.clear();
    for (EventIdx e = 0; e < ne; ++e) {
        p.post_begin[e] = static_cast<std::uint32_t>(p.posts.size());
        const Event& ev = a.events[e];
        for (const auto& [atom, cond] : ev.post_true) {
            const auto ext = s.sat(*cond);
            p.posts.push_back({e, atom, true, static_cast<std::uint32_t>(p.post_ext.size())});
            p.post_ext.insert(p.post_ext.end(), ext.begin(), ext.end());
        }
        for (const auto& [atom, cond] : ev.post_false) {
            const auto ext = s.sat(*cond);
            p.posts.push_back({e, atom, false, static_cast<std::uint32_t>(p.post_ext.size())});
            p.post_ext.insert(p.post_ext.end(), ext.begin(), ext.end());
        }
    }
    p.post_begin[ne] = static_cast<std::uint32_t>(p.posts.size());

    // First matching observability case per world.
    p.obs_case.assign(std::size_t(na) * nw, -1);
    for (AgentIdx ag = 0; ag < na && ag < a.obs_cases.size(); ++ag) {
        const auto& cases = a.obs_cases[ag];
        if (cases.empty()) continue;

        p.obs_ext.resize(cases.size() * rw);
        for (std::size_t c = 0; c < cases.size(); ++c) {
            const auto ext = s.sat(*cases[c].condition);
            std::copy(ext.begin(), ext.end(), p.obs_ext.begin() + c * rw);
        }

        auto& claimed = p.obs_claimed;
        claimed.assign(rw, 0);
        for (std::size_t c = 0; c < cases.size(); ++c) {
            for (std::uint32_t i = 0; i < rw; ++i) {
                bits::Word fresh = p.obs_ext[c * rw + i] & ~claimed[i];
                claimed[i] |= fresh;
                while (fresh) {
                    const WorldIdx w = static_cast<WorldIdx>(i * bits::kWordBits +
                                                             std::countr_zero(fresh));
                    p.obs_case[std::size_t(ag) * nw + w] = static_cast<std::int32_t>(c);
                    fresh &= fresh - 1;
                }
            }
        }
    }
}

// KD45 seriality repair.
//
// KD45 requires every R_i to be serial: ∀w ∃v. w R_i v. The product does not
// preserve seriality — (w,e) is non-serial for agent i whenever R_i(w) = ∅ or
// R^E_i(e) = ∅ — and removal cascades, because a removed world may have been
// some other world's only successor. The set of serial worlds is the greatest
// fixpoint of "every agent's row, restricted to survivors, is non-empty", which
// this computes by repeated sweeps.
//
// Returns the surviving set, or an empty set if repair emptied W*.
std::vector<bits::Word> serial_core(const EpistemicState& s) {
    std::vector<bits::Word> alive(s.rel_words, 0);
    bits::fill_all(alive, s.num_worlds);

    for (bool changed = true; changed;) {
        changed = false;
        for (WorldIdx w = 0; w < s.num_worlds; ++w) {
            if (!bits::test(alive, w)) continue;
            for (AgentIdx ag = 0; ag < s.num_agents; ++ag) {
                if (!bits::intersects(s.succ(ag, w), alive)) {
                    bits::reset(alive, w);
                    changed = true;
                    break;
                }
            }
        }
    }
    return alive;
}

} // namespace

Outcome<ProductUpdateResult>
product_update_with_map(const EpistemicState& s, const Action& a,
                        bool enforce_kd45, const WorldCapPolicy& cap) {
    const std::uint32_t nw = s.num_worlds;
    const std::uint32_t na = s.num_agents;
    const std::uint32_t ne = static_cast<std::uint32_t>(a.events.size());
    const std::uint32_t rw = s.rel_words;

    // Pessimistic bound on |W'|, checked before any allocation.
    if (!cap.allows(nw, ne))
        return pruned<ProductUpdateResult>(PruneReason::WorldCapExceeded);

    Scratch& p = scratch();
    precompute(s, a, p);

    const std::uint32_t nw_out = ne == 0 ? 0 : p.offset[ne - 1] + p.pre_count[ne - 1];
    if (nw_out == 0)
        return pruned<ProductUpdateResult>(PruneReason::Inapplicable);

    // W' and the (w,e) → idx table, block by block.
    ProductUpdateResult out;
    out.num_events = ne;
    out.pair_to_idx.assign(std::size_t(nw) * ne, kNoWorld);

    EpistemicState result;
    result.allocate(nw_out, s.num_atoms, na);

    for (EventIdx e = 0; e < ne; ++e) {
        WorldIdx idx = p.offset[e];
        bits::for_each(p.pre_of(e), [&](std::uint32_t w) {
            out.pair_to_idx[std::size_t(w) * ne + e] = idx;
            bits::copy_from(result.val(idx), s.val(w));
            ++idx;
        });

        // Postconditions; sets precede resets, as before.
        const auto pre = p.pre_of(e);
        for (std::uint32_t k = p.post_begin[e]; k < p.post_begin[e + 1]; ++k) {
            const auto& post = p.posts[k];
            const bits::ConstWordSpan guard{p.post_ext.data() + post.ext, rw};
            for (std::uint32_t i = 0; i < rw; ++i) {
                bits::Word hit = guard[i] & pre[i];
                while (hit) {
                    const WorldIdx w = static_cast<WorldIdx>(i * bits::kWordBits +
                                                             std::countr_zero(hit));
                    const auto dst = result.val(out.pair_to_idx[std::size_t(w) * ne + e]);
                    if (post.value) bits::set(dst, post.atom);
                    else            bits::reset(dst, post.atom);
                    hit &= hit - 1;
                }
            }
        }
    }

    // W'*.
    {
        auto des = result.designated_bits();
        bits::for_each(s.designated_bits(), [&](std::uint32_t w) {
            for (EventIdx e : a.designated_events) {
                if (e >= ne) continue;
                const WorldIdx idx = out.pair_to_idx[std::size_t(w) * ne + e];
                if (idx != kNoWorld) bits::set(des, idx);
            }
        });
        if (bits::empty(des))
            return pruned<ProductUpdateResult>(PruneReason::Inapplicable);
    }

    // R'_i.
    //
    //   R'_i((w,e)) = ⋃_{f ∈ R^E_i(e)}  off(f) + PEXT(R_i(w), sat(pre(f)))
    //
    // Packed rows depend on (i, w) only; an event row equal to the previous
    // one reuses the previous product row.
    const std::uint32_t ew = static_cast<std::uint32_t>(bits::words_for(ne));
    p.packed.resize(std::size_t(ne) * rw);
    p.packed_bits.resize(ne);
    p.all_events.assign(ew, 0);
    bits::fill_all(p.all_events, ne);

    for (AgentIdx ag = 0; ag < na; ++ag) {
        const auto* agent_cases =
            (ag < a.obs_cases.size()) ? &a.obs_cases[ag] : nullptr;

        for (WorldIdx w = 0; w < nw; ++w) {
            const auto world_row = s.succ(ag, w);
            if (bits::empty(world_row)) continue;

            for (EventIdx f = 0; f < ne; ++f)
                p.packed_bits[f] = static_cast<std::uint32_t>(bits::extract(
                    world_row, p.pre_of(f),
                    bits::WordSpan{p.packed.data() + std::size_t(f) * rw, rw}));

            const std::int32_t ci = p.obs_case[std::size_t(ag) * nw + w];
            const ObsCase* oc = (ci >= 0 && agent_cases) ? &(*agent_cases)[ci] : nullptr;

            bits::ConstWordSpan prev_events{};
            WorldIdx            prev_row = kNoWorld;

            for (EventIdx e = 0; e < ne; ++e) {
                const WorldIdx new_w = out.pair_to_idx[std::size_t(w) * ne + e];
                if (new_w == kNoWorld) continue;

                // No matching case: fully observant, R^E_i(e) = E.
                assert(!oc || oc->relation_words == ew);   // ObsCase::finalize ran
                const bits::ConstWordSpan events =
                    oc ? oc->event_row(e) : bits::ConstWordSpan{p.all_events};

                auto dst = result.succ(ag, new_w);
                if (prev_row != kNoWorld && bits::equal(events, prev_events)) {
                    bits::copy_from(dst, result.succ(ag, prev_row));
                    continue;
                }

                bits::for_each(events, [&](std::uint32_t f) {
                    if (p.packed_bits[f] == 0) return;
                    bits::or_shifted(dst, p.offset[f],
                                     bits::ConstWordSpan{p.packed.data() + std::size_t(f) * rw, rw},
                                     p.packed_bits[f]);
                });
                prev_events = events;
                prev_row    = new_w;
            }
        }
    }

    result.invalidate();

    // KD45 repair.
    if (enforce_kd45) {
        const auto alive = serial_core(result);

        if (bits::count(alive) != result.num_worlds) {
            std::vector<WorldIdx> remap;
            EpistemicState repaired = restrict_state(result, alive, remap);

            if (bits::empty(repaired.designated_bits()))
                return pruned<ProductUpdateResult>(PruneReason::NonSerial);

            for (WorldIdx& idx : out.pair_to_idx)
                if (idx != kNoWorld) idx = remap[idx];

            result = std::move(repaired);
        } else if (bits::empty(result.designated_bits())) {
            return pruned<ProductUpdateResult>(PruneReason::NonSerial);
        }
    }

    out.state = std::move(result);
    return ok(std::move(out));
}

Outcome<EpistemicState>
product_update(const EpistemicState& s, const Action& a,
               bool enforce_kd45, const WorldCapPolicy& cap) {
    auto res = product_update_with_map(s, a, enforce_kd45, cap);
    if (!res) return pruned<EpistemicState>(res.error());
    return ok(std::move(res->state));
}

// Sensing.
//
// For a sensing action with E_d = {e₁, …}, the branch for e_k is the epistemic
// state given that e_k fired. All branches share the product model W' and R';
// only W'*_k = { (w, e_k) | w ∈ W* } ∩ W' differs. The single shared update also
// keeps world ids coherent between branches — running the update once per event
// would compact ids independently and make the designated sets refer to
// different worlds.
std::vector<std::pair<EventIdx, EpistemicState>>
product_update_split(const EpistemicState& s, const Action& a,
                     bool enforce_kd45, const WorldCapPolicy& cap) {
    auto full = product_update_with_map(s, a, enforce_kd45, cap);
    if (!full) return {};

    const EpistemicState& model = full->state;
    const std::uint32_t   ne    = full->num_events;

    std::vector<std::pair<EventIdx, EpistemicState>> results;
    results.reserve(a.designated_events.size());

    // Deterministic branch order: designated_events is an unordered_set, and the
    // order it yields would otherwise leak into the conditional plan's branch
    // order and into AO*'s search order.
    std::vector<EventIdx> events(a.designated_events.begin(),
                                 a.designated_events.end());
    std::sort(events.begin(), events.end());

    for (EventIdx eid : events) {
        if (eid >= ne) continue;

        std::vector<bits::Word> des(model.rel_words, 0);
        bits::for_each(s.designated_bits(), [&](std::uint32_t w) {
            const WorldIdx idx = full->pair_to_idx[std::size_t(w) * ne + eid];
            if (idx != kNoWorld) bits::set(des, idx);
        });

        // An empty designated set means this outcome is inconsistent with the
        // current state: the event could not have fired in any actual world.
        if (bits::empty(des)) continue;

        EpistemicState branch = model;
        bits::copy_from(branch.designated_bits(), des);
        branch.invalidate();

        results.emplace_back(eid, std::move(branch));
    }

    return results;
}
