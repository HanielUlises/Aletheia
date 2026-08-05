#include "heuristic.hpp"

#include <algorithm>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Heuristics over satisfaction sets.
//
// Every heuristic here is a goal-decomposition estimate: it measures how far
// each unsatisfied goal conjunct is from holding, either as a 0/1 flag or as a
// fraction of the accessible worlds that still act as counterexamples. The
// numeric behaviour is unchanged from the previous implementation; what changed
// is how it is computed and that it is now deterministic.
//
// Determinism mattered. Both `ed` and `ks` cut their counterexample scan off
// after a fixed number of accessible worlds, and the old code walked
// std::unordered_set, so *which* worlds fell inside the sample — and therefore
// the heuristic value, and therefore the plan — depended on hash iteration
// order. Bit sets are traversed in ascending index order, so the same state
// always yields the same estimate.
//
// The remaining cost is one bottom-up evaluation of the goal per state, shared
// across conjuncts through the state's satisfaction cache. The old code called
// s.satisfies() per conjunct, each a fresh recursive descent, on top of a
// separate per-heuristic (formula, world) memo that could not outlive one call.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Number of accessible worlds examined before a counterexample count is
// truncated. Bounds the cost of `ed` and `ks` on wide models.
constexpr std::size_t kMaxSample = 64;

// Depth cap on projection through nested modalities.
constexpr std::size_t kMaxDepth = 4;

using WordVec = std::vector<bits::Word>;

// The worlds agent `ag` considers possible from anywhere in `designated`:
// ⋃ { R_ag(w) | w ∈ designated }.
WordVec project(const EpistemicState& s, bits::ConstWordSpan designated, AgentIdx ag) {
    WordVec out(s.rel_words, 0);
    if (ag >= s.num_agents) return out;
    bits::for_each(designated,
                   [&](std::uint32_t w) { bits::or_into(out, s.succ(ag, w)); });
    return out;
}

// Fraction of the worlds accessible from `designated` via `ag` at which `inner`
// fails, sampled in ascending world order and truncated at kMaxSample.
float counterexample_ratio(const EpistemicState& s, bits::ConstWordSpan designated,
                           AgentIdx ag, const Formula& inner) {
    if (ag >= s.num_agents) return 1.0f;

    const auto ext = s.sat(inner);

    std::size_t fails = 0, sampled = 0;
    bits::for_each_until(designated, [&](std::uint32_t w) {
        return bits::for_each_until(s.succ(ag, w), [&](std::uint32_t v) {
            if (!bits::test(ext, v)) ++fails;
            return ++sampled < kMaxSample;
        });
    });

    if (sampled == 0) return 0.0f;
    return static_cast<float>(fails) / static_cast<float>(sampled);
}

[[nodiscard]] bool holds_throughout(const EpistemicState& s,
                                    bits::ConstWordSpan designated,
                                    const Formula& f) {
    return bits::subset_of(designated, s.sat(f));
}

} // namespace

// h1 — number of designated worlds.
float WorldCountHeuristic::operator()(const EpistemicState& s,
                                      const PlanningTask&) const {
    return static_cast<float>(s.num_designated());
}

// h2 — number of unsatisfied top-level goal conjuncts.
float UnsatisfiedGoalHeuristic::operator()(const EpistemicState& s,
                                           const PlanningTask& task) const {
    const Formula& goal = *task.goal;
    const auto designated = s.designated_bits();

    if (goal.kind == FormulaKind::And) {
        float unsat = 0.0f;
        for (const auto& c : goal.children)
            if (!holds_throughout(s, designated, *c)) unsat += 1.0f;
        return unsat;
    }
    return holds_throughout(s, designated, goal) ? 0.0f : 1.0f;
}

// h3 — epistemic distance.
//
// For a belief conjunct [i]φ, instead of the 0/1 verdict `ug` gives, this counts
// what fraction of the worlds agent i considers possible are counterexamples to
// φ — a real gradient as uncertainty is resolved. Nested modalities are handled
// by projecting the designated set through the accessibility relation and
// recursing, up to kMaxDepth.
namespace {

float epistemic_distance(const EpistemicState& s, bits::ConstWordSpan designated,
                         const Formula& f, std::size_t depth) {
    if (holds_throughout(s, designated, f)) return 0.0f;

    switch (f.kind) {
    case FormulaKind::Belief: {
        if (f.agent >= s.num_agents) return 1.0f;
        const Formula& inner = *f.children[0];

        const bool nested = inner.kind == FormulaKind::Belief ||
                            inner.kind == FormulaKind::Common ||
                            inner.kind == FormulaKind::And    ||
                            inner.kind == FormulaKind::Or;

        if (depth < kMaxDepth && nested) {
            const WordVec projected = project(s, designated, f.agent);
            if (bits::empty(projected)) return 1.0f;
            return epistemic_distance(s, projected, inner, depth + 1);
        }

        return counterexample_ratio(s, designated, f.agent, inner);
    }

    case FormulaKind::Common: {
        if (f.children.empty()) return 1.0f;
        if (depth >= kMaxDepth) return 1.0f;

        // Worst agent in the group: common knowledge is no closer than its
        // furthest constituent.
        float worst = 0.0f;
        for (AgentIdx ag : f.group) {
            const WordVec projected = project(s, designated, ag);
            if (bits::empty(projected)) continue;
            worst = std::max(worst,
                             epistemic_distance(s, projected, *f.children[0], depth + 1));
        }
        return worst;
    }

    case FormulaKind::And: {
        float total = 0.0f;
        for (const auto& c : f.children)
            total += epistemic_distance(s, designated, *c, depth);
        return total;
    }

    case FormulaKind::Or: {
        // Covers Kw expanded as [i]φ ∨ [i]¬φ: credit the nearer disjunct.
        if (f.children.size() != 2) break;
        return std::min(epistemic_distance(s, designated, *f.children[0], depth),
                        epistemic_distance(s, designated, *f.children[1], depth));
    }

    case FormulaKind::Kw: {
        if (f.agent >= s.num_agents) return 1.0f;
        const Formula& inner = *f.children[0];
        // Distance to knowing φ, or to knowing ¬φ, whichever is nearer.
        const float to_true  = counterexample_ratio(s, designated, f.agent, inner);
        return std::min(to_true, 1.0f - to_true);
    }

    default:
        break;
    }

    return 1.0f;   // unsatisfied and structurally opaque
}

} // namespace

float EpistemicDistanceHeuristic::operator()(const EpistemicState& s,
                                             const PlanningTask& task) const {
    const Formula& goal = *task.goal;
    const auto designated = s.designated_bits();

    if (goal.kind == FormulaKind::And) {
        float total = 0.0f;
        for (const auto& c : goal.children)
            total += epistemic_distance(s, designated, *c, 0);
        return total;
    }
    return epistemic_distance(s, designated, goal, 0);
}

// h4 — knowledge spread.
//
// Aimed at goals that are conjunctions of Kw formulas across agents (Gossip,
// Grapevine). Each unsatisfied conjunct contributes the fraction of the agent's
// accessible worlds that still fail to resolve it, so the value falls smoothly
// as knowledge propagates through the agent graph rather than dropping in
// whole-conjunct steps.
namespace {

float knowledge_spread(const EpistemicState& s, const Formula& f) {
    const auto designated = s.designated_bits();
    if (holds_throughout(s, designated, f)) return 0.0f;

    switch (f.kind) {
    case FormulaKind::Or:
        // Kw.box expanded to [i]φ ∨ [i]¬φ: whichever direction is nearer.
        if (f.children.size() == 2)
            return std::min(knowledge_spread(s, *f.children[0]),
                            knowledge_spread(s, *f.children[1]));
        break;

    case FormulaKind::Kw: {
        const float to_true =
            counterexample_ratio(s, designated, f.agent, *f.children[0]);
        return std::min(to_true, 1.0f - to_true);
    }

    case FormulaKind::Belief:
        return counterexample_ratio(s, designated, f.agent, *f.children[0]);

    case FormulaKind::And: {
        float total = 0.0f;
        for (const auto& c : f.children) total += knowledge_spread(s, *c);
        return total;
    }

    default:
        break;
    }

    return 1.0f;
}

} // namespace

float KnowledgeSpreadHeuristic::operator()(const EpistemicState& s,
                                           const PlanningTask& task) const {
    const Formula& goal = *task.goal;

    if (goal.kind == FormulaKind::And) {
        float total = 0.0f;
        for (const auto& c : goal.children) total += knowledge_spread(s, *c);
        return total;
    }
    return knowledge_spread(s, goal);
}
