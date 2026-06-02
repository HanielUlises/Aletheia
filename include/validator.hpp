#pragma once
#include "search.hpp"
#include "task.hpp"
#include <string>
#include <vector>

struct ValidationResult {
    bool valid{false};
    std::string error;
    size_t branches_checked{0};
    size_t leaves_reached{0};
};

// Replay a conditional plan tree against the task.
// Checks that:
//   - every action is applicable in the state it is applied to
//   - every sensing branch matches an event that fires in that state
//   - every leaf (null subtree) satisfies the goal
ValidationResult validate(const PlanningTask& task,
                          const std::shared_ptr<PlanNode>& plan_tree);
