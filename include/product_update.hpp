#pragma once
#include "state.hpp"
#include "action.hpp"
#include <vector>
#include <unordered_map>

// Internal result of a DEL product update, the updated state together with
// the (world,event)->new_world_id mapping that was used to build it.
//

struct ProductUpdateResult {
    EpistemicState state;
    // Maps encode_pair(original_world_id, event_id) -> new world id in state.
    // After KD45 seriality repair the new world ids are compacted so that
    // worlds[i].id == i always holds; the map already reflects the compacted ids.
    std::unordered_map<uint64_t, WorldIdx> pair_to_idx;
};

// Compute s ⊗ a  (DEL product update).
//
// Returns the updated epistemic state, or std::nullopt if the action
// is not applicable (no designated event's precondition holds in any
// designated world).
#include "world_cap_policy.hpp"

std::optional<EpistemicState> product_update(const EpistemicState& s,
                                              const Action& a,
                                              bool enforce_kd45 = false,
                                              const WorldCapPolicy& cap = make_world_cap_policy(false));

std::optional<ProductUpdateResult>
product_update_with_map(const EpistemicState& s, const Action& a,
                        bool enforce_kd45 = false,
                        const WorldCapPolicy& cap = make_world_cap_policy(false));

std::vector<std::pair<EventIdx, EpistemicState>>
product_update_split(const EpistemicState& s, const Action& a,
                     bool enforce_kd45 = false,
                     const WorldCapPolicy& cap = make_world_cap_policy(false));