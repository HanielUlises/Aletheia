#pragma once
#include "task.hpp"

#include <vector>

// Agent symmetry.
//
// A swap (a b) is a task symmetry when renaming agents a ↔ b, and the atoms and
// actions whose names mention them, maps the initial state, every action and
// the goal onto themselves. The swaps that also fix a state s generate
// Sym(C_1) × … × Sym(C_k) ⊆ Stab(s) over the connected components C of the swap
// graph. Applying σ(a) at s is then equivalent to applying a, up to σ, so only
// one action per orbit needs expanding: plans through the others are images of
// plans through it. This preserves completeness and optimal plan length.
struct AgentSymmetry {
    struct Swap {
        AgentIdx               a{0}, b{0};
        std::vector<AtomIdx>   atom;     // atom → renamed atom
        std::vector<ActionIdx> action;   // action → renamed action
    };

    std::vector<Swap> swaps;

    // Agents named in each action, in order of first appearance.
    std::vector<std::vector<AgentIdx>> action_agents;

    [[nodiscard]] static AgentSymmetry detect(const PlanningTask& task);
    [[nodiscard]] bool empty() const noexcept { return swaps.empty(); }
};

// Components of the swap graph restricted to swaps that fix one state.
struct StateSymmetry {
    std::vector<std::uint32_t> component;   // agent → component id
    std::vector<AgentIdx>      members;     // agents, grouped by component, ascending
    std::vector<std::uint32_t> begin;       // component → range into members
    bool                       trivial{true};

    // True iff `action` is its orbit's representative: within every component,
    // the agents it names appear as that component's smallest members, in order.
    [[nodiscard]] bool keep(const AgentSymmetry& sym, ActionIdx action) const;
};

// `s` must be canonical (produced by bisim_contract).
[[nodiscard]] StateSymmetry stabiliser(const AgentSymmetry& sym, const EpistemicState& s);
