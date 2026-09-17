#pragma once
#include "heuristic.hpp"
#include "search.hpp"

#include <optional>
#include <string>

// Parallel portfolio: runs every search configuration compatible with the task
// in its own thread and keeps the first plan found; the others are cancelled.
// Compatibility is logical (contingent members only for tasks with sensing or
// non-deterministic actions), not a tuned threshold.
struct PortfolioOutcome {
    std::string                            member;       // empty if none succeeded
    std::optional<SearchResult>            linear;
    std::optional<ConditionalSearchResult> contingent;
    bool                                   unsolvable{false};   // AO* exhausted (no policy)
};

[[nodiscard]] PortfolioOutcome race(const PlanningTask& task, const Heuristic& relaxation,
                                    const Heuristic& spread, Deadline deadline);
