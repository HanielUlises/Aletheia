#pragma once
#include "heuristic.hpp"

#include <cstdint>
#include <unordered_map>
#include <vector>

// kadd: additive heuristic over a delete-free relaxation of knowledge.
//
// Facts are literals, knowledge literals [j]ℓ, and the modal formulas the goal
// and preconditions mention; Kw_j p is derived from [j]p ∨ [j]¬p. Relaxed
// operators come from the event models:
//
//   - an unconditional postcondition of event e adds its literal;
//   - agent j, in an observability case whose event row from e contains only
//     events after which literal ℓ holds (it is a precondition conjunct not
//     overwritten, or a postcondition), gains [j]ℓ under pre(e) ∧ case condition.
//
// So seeing an event fully teaches its preconditions, seeing it partially
// teaches only what all indistinguishable events share, and not seeing it
// (nil) teaches nothing. A fact holds initially if it holds at some designated
// world, which keeps the estimate meaningful inside sensing branches.
//
// Preconditions compile to one And/Or DAG shared across actions through formula
// interning; facts and operators that cannot reach the goal are dropped.
class KnowledgeRelaxationHeuristic : public Heuristic {
public:
    explicit KnowledgeRelaxationHeuristic(const PlanningTask& task);

    float operator()(const EpistemicState& s, const PlanningTask& task) const override;

    // Applicable actions of a relaxed plan extracted from the cost fixpoint.
    bool preferred(const EpistemicState& s, const PlanningTask& task,
                   std::vector<ActionIdx>& out) const override;

private:
    enum class Kind : std::uint8_t { True, False, Fact, And, Or };

    struct Req {
        Kind          kind{Kind::True};
        std::uint32_t fact{0};             // Kind::Fact
        std::uint32_t begin{0}, end{0};    // children, into req_children_
    };

    struct Op {
        std::uint32_t pre{0};              // requirement
        std::uint32_t begin{0}, end{0};    // added facts, into op_adds_
        ActionIdx     action{0};
    };

    std::uint32_t compile(const FormulaPtr& f, bool negated);
    std::uint32_t fact(const FormulaPtr& f);
    std::uint32_t node(Kind k, std::vector<std::uint32_t> children);
    void          add_op(std::uint32_t pre, const std::vector<std::uint32_t>& adds);
    void          prune();
    // Cost fixpoint at s; supporter[f] is the operator giving fact f its cost,
    // or -1 when initial or derived.
    void          costs(const EpistemicState& s, std::vector<std::int32_t>& fc,
                        std::vector<std::int32_t>& rc, std::vector<std::int32_t>* supporter) const;

    ActionIdx     current_action_{0};

    std::vector<Req>           reqs_;
    std::vector<std::uint32_t> req_children_;
    std::unordered_map<std::uint64_t, std::uint32_t> compiled_;   // (formula id, polarity)

    std::vector<FormulaPtr>    facts_;
    std::unordered_map<std::uint32_t, std::uint32_t> fact_of_;    // formula id → fact
    std::vector<std::int32_t>  derived_;                           // fact → requirement, or -1

    std::vector<Op>            ops_;
    std::vector<std::uint32_t> op_adds_;

    std::vector<std::uint32_t> goal_;                              // one requirement per conjunct
};
