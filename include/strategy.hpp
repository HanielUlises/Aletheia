#pragma once
#include "heuristic.hpp"
#include "task.hpp"

#include <memory>
#include <optional>
#include <string>

// Labels to searches, for every program that turns a policy decision or a
// command-line flag into a run: the binary, and anything linking the library.
// The selection policy names heuristics and strategies by these labels, so a
// label the policy can choose and a program cannot build is a task that fails
// to plan. Keeping one table is what keeps the two in step.

enum class Strategy { GBFS, EHC, AOSTAR, REPLAN, PORTFOLIO };

// The heuristic a label names, or nullptr for a label that names none.
[[nodiscard]] std::unique_ptr<Heuristic> make_heuristic(const std::string& label,
                                                        const PlanningTask& task);

// Long-form names for the log. The policy speaks in short labels, but the run
// logs are a committed artefact and readers know them by these names.
[[nodiscard]] const char* heuristic_display(const std::string& label);

[[nodiscard]] std::optional<Strategy> parse_strategy(const std::string& label);

[[nodiscard]] const char* strategy_name(Strategy s);

// True when some action has more than one designated event, and so can branch.
[[nodiscard]] bool has_sensing_actions(const PlanningTask& task);
