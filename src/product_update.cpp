#include "product_update.hpp"
#include <algorithm>
#include <iostream>

// DEL Product Update  s ⊗ a
//
// Given a Kripke model M = (W, {R_i}, V, W*) and an event model
// A = (E, {R^E_i}, pre, post, E_d), the product update produces:
//
//   W'         = { (w,e) | w ∈ W, e ∈ E, M,w ⊨ pre(e) }
//   R'_i       = { ((w,e),(v,f)) | (w,v) ∈ R_i  ∧  (e,f) ∈ R^E_i }
//   V'(w,e)(p) = post(e)(p) if p ∈ dom(post(e)), else V(w)(p)
//   W'*        = { (w,e) | w ∈ W*, e ∈ E_d }
//
// Postconditions are split into post_true / post_false maps conditioned on
// a formula: atom p becomes true at (w,e) iff post_true[p] holds at w in M
// (and symmetrically for post_false). Atoms not in either map are copied
// from V(w) unchanged.
//
// The event model relation R^E_i is encoded per-world as a conditional
// observability structure (ObsCase): a list of (condition, event_relation)
// pairs evaluated in order at each world w. The first matching case gives the
// row R^E_i(e) for that world. If no case matches we fall back to full
// observability (agent sees all events), i.e. R^E_i(e) = E for all e.

// Pre-contraction world cap.
//
// The raw product W × E can grow as |W| * |E| before any precondition
// filtering or bisimulation contraction. For private-announcement actions the
// nil event has pre(nil) = ⊤, so it fires on every world — the pre-contraction
// size is exactly |W| * |E| in the worst case.
//
// Without a cap, GBFS queues states at depth k with up to |W₀| * 2^k worlds
// and O((|W₀| * 2^k)^2 * na) accessibility pairs each. For gossip (|W₀|=32,
// na=5) this reaches ~1.3 billion pairs by step 9, exhausting RAM before
// bisim_contract can reduce anything.
//
// The cap is checked against the pessimistic upper bound |W| * |E| before any
// precondition filtering. If it fires, we return nullopt — the branch is pruned
// from the search. This is sound: any plan that passes through a state requiring
// >WORLD_CAP pre-contraction worlds can be found via a different prefix where
// bisim_contract kept the model small enough.
//
// 512 gives headroom for all current benchmarks: the largest initial model has
// 32 worlds and |E|=2, so the first step produces 64 raw worlds. A plan of
// length 8 through a sequence of fully-contracting actions stays well under 64
// worlds post-contraction; only diverging (non-contracting) paths hit the cap.
static constexpr size_t WORLD_CAP = 512;

static uint64_t encode_pair(WorldIdx w, EventIdx e) {
    return (static_cast<uint64_t>(w) << 32) | static_cast<uint64_t>(e);
}

// KD45 seriality repair.
//
// KD45 requires every accessibility relation to be serial: ∀w ∃v: w R_i v.
// The product update does not guarantee this. A world (w,e) in W' has:
//
//   R'_i((w,e)) = { (v,f) | v ∈ R_i(w), f ∈ R^E_i(e) }
//
// If R_i(w) = ∅ or R^E_i(e) = ∅ for any agent i, the world (w,e) is
// non-serial and must be removed. Removal can cascade: a world (v,f) that
// was the sole R_i-successor of some (u,g) may be removed, making (u,g)
// non-serial in turn. We iterate to a fixpoint.
//
// Non-seriality also arises from KD45 seriality repair in the *source* model
// s: if s was produced by a prior product update + seriality repair, some
// worlds in s may already have had their accessibility trimmed, and the
// cross-product rows in the result inherit those trimmed sets.
//
// After the fixpoint, world IDs are compacted so that worlds[i].id == i,
// which is the invariant expected by holds_at and hash. The pair_to_idx map
// is patched in place so that callers (product_update_split) see compacted IDs
// without needing to re-derive the mapping.
//
// Returns false iff all designated worlds were removed (action not applicable
// in any actual world after repair); the caller returns nullopt.
static bool enforce_seriality(EpistemicState& s,
                               std::unordered_map<uint64_t, WorldIdx>& pair_to_idx) {
    size_t na = s.num_agents;
    bool changed = true;

    while (changed) {
        changed = false;

        std::unordered_set<WorldIdx> non_serial;
        for (auto& world : s.worlds) {
            WorldIdx w = world.id;
            for (AgentIdx ag = 0; ag < na; ag++) {
                if (s.accessibility[ag][w].empty()) {
                    non_serial.insert(w);
                    break;
                }
            }
        }

        if (non_serial.empty()) break;

        std::unordered_set<WorldIdx> keep;
        for (auto& world : s.worlds)
            if (!non_serial.count(world.id))
                keep.insert(world.id);

        // Trim successor sets: any pointer to a non-serial world is invalid
        // because that world is being removed. We only retain successors in
        // `keep`. A row that shrinks here can make the owning world non-serial
        // in the next iteration — this is how cascading removal is handled.
        for (AgentIdx ag = 0; ag < na; ag++) {
            for (auto& world : s.worlds) {
                WorldIdx w = world.id;
                auto& row = s.accessibility[ag][w];
                std::unordered_set<WorldIdx> pruned;
                for (WorldIdx v : row)
                    if (keep.count(v))
                        pruned.insert(v);
                if (pruned.size() != row.size()) {
                    row = std::move(pruned);
                    changed = true;
                }
            }
        }

        s.worlds.erase(
            std::remove_if(s.worlds.begin(), s.worlds.end(),
                [&](const World& w){ return non_serial.count(w.id) > 0; }),
            s.worlds.end());

        for (WorldIdx w : non_serial)
            s.designated.erase(w);

        if (s.designated.empty()) return false;
    }

    // Compact world IDs.
    //
    // After erasure, worlds[i].id is no longer guaranteed to equal i because
    // the vector was built from the original product with sequential IDs and
    // then had arbitrary elements removed. We remap old IDs to their new
    // vector positions and patch everything that stores a WorldIdx: world
    // structs, accessibility rows, designated set, and pair_to_idx.
    //
    // pair_to_idx entries whose world was removed (id not in remap) are left
    // with stale IDs — product_update_split filters them via the survived_worlds
    // set so they never produce branch states.
    {
        std::unordered_map<WorldIdx, WorldIdx> remap;
        remap.reserve(s.worlds.size());
        for (WorldIdx i = 0; i < static_cast<WorldIdx>(s.worlds.size()); i++)
            remap[s.worlds[i].id] = i;

        bool needs_compact = false;
        for (auto& [old_id, new_id] : remap)
            if (old_id != new_id) { needs_compact = true; break; }

        if (needs_compact) {
            for (WorldIdx i = 0; i < static_cast<WorldIdx>(s.worlds.size()); i++)
                s.worlds[i].id = i;

            size_t new_nw = s.worlds.size();
            std::vector<Relation> new_acc(na, Relation(new_nw));
            for (AgentIdx ag = 0; ag < na; ag++) {
                for (auto& [old_id, new_id] : remap) {
                    auto& old_row = s.accessibility[ag][old_id];
                    auto& new_row = new_acc[ag][new_id];
                    for (WorldIdx v : old_row) {
                        auto it = remap.find(v);
                        if (it != remap.end())
                            new_row.insert(it->second);
                    }
                }
            }
            s.accessibility = std::move(new_acc);

            std::unordered_set<WorldIdx> new_des;
            for (WorldIdx w : s.designated) {
                auto it = remap.find(w);
                if (it != remap.end())
                    new_des.insert(it->second);
            }
            s.designated = std::move(new_des);

            for (auto& [key, old_wid] : pair_to_idx) {
                auto it = remap.find(old_wid);
                if (it != remap.end())
                    old_wid = it->second;
            }
        }
    }

    return !s.designated.empty();
}

// Core product update returning both the updated state and the pair→world map.
//
// pair_to_idx maps encode_pair(w, e) → new_world_id for every (w,e) pair
// that passed precondition filtering. This mapping is the single authoritative
// source for world identity: product_update_split reuses it verbatim to
// assign designated sets for each sensing branch. Re-deriving it independently
// (as an earlier version did) introduced world-ID inconsistencies when
// seriality repair compacted the IDs between the two derivations.
std::optional<ProductUpdateResult>
product_update_with_map(const EpistemicState& s, const Action& a,
                        bool enforce_kd45) {
    size_t na = s.num_agents;

    // Upper-bound check before any allocation. |W| * |E| is the maximum number
    // of worlds the product can produce (all preconditions true everywhere).
    // Actual count is ≤ this after precondition filtering, but the check is
    // conservative by design — we want to abort before building the world
    // vector, not after.
    if (s.worlds.size() * a.events.size() > WORLD_CAP) {
        std::cerr << "[product_update] world cap hit ("
                  << s.worlds.size() << " x " << a.events.size()
                  << " > " << WORLD_CAP << ") — pruning branch\n";
        return std::nullopt;
    }

    ProductUpdateResult out;
    EpistemicState& result = out.state;
    auto& pair_to_idx = out.pair_to_idx;

    result.num_agents = na;

    // Build W': iterate all (w,e) pairs and retain those where pre(e) holds
    // at w. For each surviving pair, apply postconditions to produce V'(w,e).
    // post_true / post_false are conditional: the atom flip only happens if
    // the associated formula holds at w in the *original* model M (not the
    // updated one — postconditions are evaluated in the pre-update state).
    for (auto& world : s.worlds) {
        for (auto& event : a.events) {
            if (!s.holds_at(*event.precondition, world.id))
                continue;

            WorldIdx new_id = static_cast<WorldIdx>(result.worlds.size());
            pair_to_idx[encode_pair(world.id, event.id)] = new_id;

            World new_world;
            new_world.id    = new_id;
            new_world.atoms = world.atoms;

            for (auto& [atom, cond] : event.post_true) {
                if (s.holds_at(*cond, world.id))
                    new_world.atoms.insert(atom);
            }
            for (auto& [atom, cond] : event.post_false) {
                if (s.holds_at(*cond, world.id))
                    new_world.atoms.erase(atom);
            }

            result.worlds.push_back(std::move(new_world));
        }
    }

    // Build W'*: designated worlds in the result are (w,e) where w ∈ W* and
    // e ∈ E_d AND (w,e) survived precondition filtering (i.e. is in pair_to_idx).
    // A designated event whose precondition fails at every actual world makes
    // the action inapplicable (result.designated ends up empty → nullopt).
    for (WorldIdx w : s.designated) {
        for (EventIdx e : a.designated_events) {
            auto key = encode_pair(w, e);
            auto it = pair_to_idx.find(key);
            if (it != pair_to_idx.end())
                result.designated.insert(it->second);
        }
    }

    if (result.designated.empty()) return std::nullopt;

    // Build R'_i for all agents.
    //
    // For each surviving pair (w,e) → new_w, the successor set R'_i(new_w) is
    // the cross-product of R_i(w) and R^E_i(e):
    //
    //   R'_i(new_w) = { pair_to_idx(v,f) | v ∈ R_i(w), f ∈ R^E_i(e) }
    //
    // R^E_i(e) comes from the ObsCase list for agent i at world w: we walk the
    // cases in order and take the first whose condition holds. This implements
    // conditional (partial) observability — different worlds can put the same
    // agent in different epistemic positions w.r.t. the same event.
    //
    // If no obs case matches (condition list exhausted), we default to full
    // observability: R^E_i(e) = E, i.e. the agent considers all events possible.
    // This fallback is intentionally conservative — it produces more successors
    // than necessary, which is safe (it over-approximates uncertainty) but may
    // slow contraction.
    size_t nw_new = result.worlds.size();
    result.accessibility.resize(na, Relation(nw_new));

    for (AgentIdx ag = 0; ag < na; ag++) {
        for (auto& [pair_we, new_w] : pair_to_idx) {
            WorldIdx w = static_cast<WorldIdx>(pair_we >> 32);
            EventIdx e = static_cast<EventIdx>(pair_we & 0xFFFFFFFF);

            const RelRow& world_row = s.accessibility[ag][w];

            const std::vector<std::unordered_set<EventIdx>>* event_rel = nullptr;
            if (ag < a.obs_cases.size()) {
                for (auto& oc : a.obs_cases[ag]) {
                    if (s.holds_at(*oc.condition, w)) {
                        event_rel = &oc.relation;
                        break;
                    }
                }
            }

            if (event_rel) {
                const auto& event_row = (*event_rel)[e];
                for (WorldIdx v : world_row) {
                    for (EventIdx f : event_row) {
                        auto key2 = encode_pair(v, f);
                        auto it2 = pair_to_idx.find(key2);
                        if (it2 != pair_to_idx.end())
                            result.accessibility[ag][new_w].insert(it2->second);
                    }
                }
            } else {
                // Full-observability fallback: cross with all events.
                // Only pairs that survived precondition filtering appear in
                // pair_to_idx, so this naturally excludes (v,f) pairs where
                // pre(f) failed at v even though R^E_i includes f.
                for (WorldIdx v : world_row) {
                    for (auto& event : a.events) {
                        auto key2 = encode_pair(v, event.id);
                        auto it2 = pair_to_idx.find(key2);
                        if (it2 != pair_to_idx.end())
                            result.accessibility[ag][new_w].insert(it2->second);
                    }
                }
            }
        }
    }

    // enforce_seriality patches pair_to_idx in place so that the compacted
    // world IDs it contains after repair are consistent with result.worlds.
    // product_update_split depends on this — it reads pair_to_idx after this
    // call and must see the post-compaction IDs.
    if (enforce_kd45) {
        if (!enforce_seriality(result, pair_to_idx))
            return std::nullopt;
    }

    return out;
}

std::optional<EpistemicState> product_update(const EpistemicState& s,
                                              const Action& a,
                                              bool enforce_kd45) {
    auto res = product_update_with_map(s, a, enforce_kd45);
    if (!res) return std::nullopt;
    return std::move(res->state);
}

// Sensing product update: one EpistemicState per designated event.
//
// For a sensing action with E_d = {e₁, e₂, ...}, each branch represents the
// epistemic state of the world *given that event eₖ fired*. The branch for eₖ
// shares the full product W' and accessibility R' with the other branches —
// what differs is only the designated set: W'*_k = { (w, eₖ) | w ∈ W* } ∩ W'.
//
// This sharing is correct because all agents observe the full product update;
// the branch just restricts which worlds the planner considers "actual" for
// the subtree rooted at this sensing outcome.
//
// We run product_update_with_map once and reuse its pair_to_idx to derive each
// branch's designated set. The alternative — running product_update_with_map
// once per event — would produce states with independently compacted world IDs,
// making the branch designated sets incoherent with each other (a world id in
// one branch would refer to a different world in another).
std::vector<std::pair<EventIdx, EpistemicState>>
product_update_split(const EpistemicState& s, const Action& a, bool enforce_kd45) {
    auto maybe_full = product_update_with_map(s, a, enforce_kd45);
    if (!maybe_full) return {};

    const EpistemicState& full  = maybe_full->state;
    const auto& pair_to_idx     = maybe_full->pair_to_idx;

    // survived_worlds contains the world IDs that exist in the post-repair,
    // post-compaction state. pair_to_idx may still contain entries pointing to
    // world IDs that were removed by seriality repair (their stale IDs were not
    // deleted from the map, only patched for surviving worlds). We filter those
    // out here by checking membership in survived_worlds.
    std::unordered_set<WorldIdx> survived;
    survived.reserve(full.worlds.size());
    for (auto& fw : full.worlds)
        survived.insert(fw.id);

    std::vector<std::pair<EventIdx, EpistemicState>> results;

    for (EventIdx eid : a.designated_events) {
        std::unordered_set<WorldIdx> branch_designated;

        for (WorldIdx w : s.designated) {
            auto key = encode_pair(w, eid);
            auto it  = pair_to_idx.find(key);
            // (w, eid) either failed the precondition check (not in pair_to_idx)
            // or was removed by seriality repair (not in survived). Either way
            // this world does not contribute to the branch's actual worlds.
            if (it == pair_to_idx.end()) continue;
            if (!survived.count(it->second)) continue;
            branch_designated.insert(it->second);
        }

        // An empty branch_designated means this sensing outcome is inconsistent
        // with the current epistemic state — the event could not have fired in
        // any actual world. We skip it rather than emitting an empty state.
        if (branch_designated.empty()) continue;

        EpistemicState branch;
        branch.num_agents    = full.num_agents;
        branch.worlds        = full.worlds;
        branch.accessibility = full.accessibility;
        branch.designated    = std::move(branch_designated);

        results.emplace_back(eid, std::move(branch));
    }

    return results;
}