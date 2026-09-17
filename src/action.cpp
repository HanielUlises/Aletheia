#include "action.hpp"
#include "state.hpp"

// Applicability reduces to two set tests once preconditions are evaluated as
// extensions over the whole model. sat(pre(e)) is computed once and memoised on
// the state, and the product update that follows reuses it; evaluating the
// precondition per designated world would instead re-descend the formula once
// for each world tested.

// Strong, conformant applicability.
//
// Ontic actions (|E_d| = 1) require W* ⊆ sat(pre(e)). An ontic action fired
// when only some designated worlds satisfy the precondition would silently drop
// the others from W'*, producing a state that conflates partial execution with
// full execution and can report spurious goal satisfaction.
//
// Sensing actions (|E_d| ≥ 2) require only W* ∩ sat(pre(e)) ≠ ∅ for some
// designated event. They branch through product_update_split, so worlds where
// different events fire land in separate subtrees and no conflation occurs.
// Every designated world satisfies the precondition of some designated event,
// as in plank. For an ontic action this is W* ⊆ sat(pre(e)).
bool Action::applicable(const EpistemicState& s) const {
    if (designated_events.empty()) return false;
    if (bits::empty(s.designated_bits())) return false;

    if (designated_events.size() == 1) {
        const EventIdx eid = *designated_events.begin();
        if (eid >= events.size()) return false;
        return bits::subset_of(s.designated_bits(), s.sat(*events[eid].precondition));
    }

    std::vector<bits::Word> covered(s.rel_words, 0);
    for (EventIdx eid : designated_events)
        if (eid < events.size()) bits::or_into(covered, s.sat(*events[eid].precondition));
    return bits::subset_of(s.designated_bits(), covered);
}

// Existential applicability, used only to shortlist actions for heuristic
// ranking. Never decides whether a successor is generated.
bool Action::applicable_weak(const EpistemicState& s) const {
    if (designated_events.empty()) return false;

    for (EventIdx eid : designated_events) {
        if (eid >= events.size()) continue;
        if (bits::intersects(s.designated_bits(),
                             s.sat(*events[eid].precondition)))
            return true;
    }
    return false;
}
