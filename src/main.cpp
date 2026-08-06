#include "parser.hpp"
#include "validator.hpp"
#include "search.hpp"
#include "heuristic.hpp"
#include "selection_policy.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <stdexcept>
#include <memory>
#include <chrono>
#include <algorithm>
#include <optional>

static void write_plan_tree(std::ostream& out,
                            const std::shared_ptr<PlanNode>& node,
                            int indent = 0) {
    std::string pad(indent * 2, ' ');
    std::string pad2((indent + 1) * 2, ' ');
    std::string pad3((indent + 2) * 2, ' ');

    if (!node) {
        out << "null";
        return;
    }

    out << "{\n";
    out << pad2 << "\"action\": \"" << node->action << "\",\n";
    out << pad2 << "\"branches\": [\n";

    for (size_t i = 0; i < node->branches.size(); i++) {
        auto& [eid, child] = node->branches[i];

        out << pad3 << "{\n";
        out << pad3 << "  \"event\": " << eid << ",\n";
        out << pad3 << "  \"subtree\": ";
        write_plan_tree(out, child, indent + 3);
        out << "\n" << pad3 << "}";

        if (i + 1 < node->branches.size())
            out << ",";

        out << "\n";
    }

    out << pad2 << "]\n";
    out << pad << "}";
}

static void write_linear_plan(std::ostream& out,
                              const SearchResult& result) {
    out << "[";
    for (size_t i = 0; i < result.plan.size(); i++) {
        if (i > 0) out << ", ";
        out << "\"" << result.plan[i] << "\"";
    }
    out << "]\n";
}

enum class Strategy { GBFS, EHC, AOSTAR };

static bool has_sensing_actions(const PlanningTask& task) {
    for (auto& action : task.actions)
        if (action.designated_events.size() > 1)
            return true;
    return false;
}

static std::unique_ptr<Heuristic> make_heuristic(const std::string& label) {
    if (label == "ug")   return std::make_unique<UnsatisfiedGoalHeuristic>();
    if (label == "ed")   return std::make_unique<EpistemicDistanceHeuristic>();
    if (label == "ks")   return std::make_unique<KnowledgeSpreadHeuristic>();
    if (label == "wc")   return std::make_unique<WorldCountHeuristic>();
    if (label == "rpg")  return std::make_unique<RelaxedClosureHeuristic>(RelaxedAggregation::Max);
    if (label == "radd") return std::make_unique<RelaxedClosureHeuristic>(RelaxedAggregation::Add);
    return nullptr;
}

// Long-form names for the log. The policy speaks in short labels, but the run
// logs are a committed artefact and readers know them by these names.
static const char* heuristic_display(const std::string& label) {
    if (label == "ug")   return "unsatisfied-goal";
    if (label == "ed")   return "epistemic-distance";
    if (label == "ks")   return "knowledge-spread";
    if (label == "wc")   return "world-count";
    if (label == "rpg")  return "relaxed-closure (max)";
    if (label == "radd") return "relaxed-closure (add)";
    return "unknown";  // unreachable: make_heuristic rejects the label first
}

static std::optional<Strategy> parse_strategy(const std::string& label) {
    if (label == "gbfs")   return Strategy::GBFS;
    if (label == "ehc")    return Strategy::EHC;
    if (label == "aostar") return Strategy::AOSTAR;
    return std::nullopt;
}

static const char* strategy_name(Strategy s) {
    switch (s) {
        case Strategy::AOSTAR: return "AO*";
        case Strategy::EHC:    return "EHC";
        default:               return "GBFS";
    }
}

static void usage(const char* prog) {
    std::cerr
        << "Usage:\n"
        << "  " << prog
        << " --task <task.json> --plan <plan.json> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --task         Path to grounded JSON task\n"
        << "  --plan         Output plan file\n"
        << "  --heuristic    ug | ed | ks | wc | rpg | radd  (default: auto)\n"
        << "  --strategy     gbfs | ehc | aostar             (default: auto)\n"
        << "  --policy       Selection-policy JSON; overrides the built-in\n"
        << "                 rules used to auto-select strategy and heuristic\n"
        << "  --print-policy Write the effective policy to stdout and exit\n"
        << "  --explain      Report which rule decided each auto-selection\n"
        << "  --limit        Max nodes / max depth (0 = unlimited)\n"
        << "  --timeout      Timeout in seconds (AO* only)\n"
        << "  --ehc          Force EHC (alias for --strategy ehc)\n"
        << "  --gbfs         Force GBFS (alias for --strategy gbfs)\n"
        << "  --conditional  Force AO* (alias for --strategy aostar)\n"
        << "  --help         Show this message\n";
}

int main(int argc, char* argv[]) {

    std::string task_path;
    std::string plan_path;
    std::string heuristic_name;  // empty = auto
    std::string strategy_name_arg;
    std::string policy_path;

    size_t limit        = 0;
    size_t timeout_secs = 0;

    bool print_policy = false;
    bool explain      = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if      (arg == "--task"      && i+1 < argc) task_path      = argv[++i];
        else if (arg == "--plan"      && i+1 < argc) plan_path      = argv[++i];
        else if (arg == "--heuristic" && i+1 < argc) heuristic_name = argv[++i];
        else if (arg == "--strategy"  && i+1 < argc) strategy_name_arg = argv[++i];
        else if (arg == "--policy"    && i+1 < argc) policy_path    = argv[++i];
        else if (arg == "--limit"     && i+1 < argc) limit          = std::stoul(argv[++i]);
        else if (arg == "--timeout"   && i+1 < argc) timeout_secs   = std::stoul(argv[++i]);
        else if (arg == "--print-policy") print_policy      = true;
        else if (arg == "--explain")      explain           = true;
        else if (arg == "--conditional")  strategy_name_arg = "aostar";
        else if (arg == "--ehc")          strategy_name_arg = "ehc";
        else if (arg == "--gbfs")         strategy_name_arg = "gbfs";
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    // Load the policy before anything else: a malformed one is a usage error,
    // and reporting it after a long parse would be needlessly late.
    SelectionPolicy policy;
    try {
        policy = policy_path.empty() ? SelectionPolicy::builtin()
                                     : SelectionPolicy::load(policy_path);
    } catch (const std::exception& e) {
        std::cerr << "Error in selection policy: " << e.what() << "\n";
        return 1;
    }

    if (print_policy) {
        std::cout << policy.to_json() << "\n";
        return 0;
    }

    if (task_path.empty() || plan_path.empty()) {
        std::cerr << "Error: --task and --plan are required.\n";
        usage(argv[0]);
        return 1;
    }

    PlanningTask task;
    try {
        task = load_task(task_path);
    } catch (const std::exception& e) {
        std::cerr << "Error loading task: " << e.what() << "\n";
        return 1;
    }

    const TaskFeatures features = TaskFeatures::extract(task);

    if (explain) {
        std::cerr << "[main] Features:";
        for (auto& n : TaskFeatures::names())
            std::cerr << ' ' << n << '=' << *features.lookup(n);
        std::cerr << "\n";
    }

    // Heuristic selection: an explicit flag overrides the policy.
    std::string heuristic_label = heuristic_name;
    std::string heuristic_rule;

    if (heuristic_label.empty()) {
        Decision d    = select(policy.heuristic_rules, features);
        heuristic_label = d.outcome;
        heuristic_rule  = d.rule;
    }

    auto h = make_heuristic(heuristic_label);
    if (!h) {
        std::cerr << "Error: unknown heuristic '" << heuristic_label << "'; expected one of:";
        for (auto& l : heuristic_labels()) std::cerr << ' ' << l;
        std::cerr << "\n";
        return 1;
    }

    std::cerr << "[main] Heuristic: " << heuristic_display(heuristic_label);
    if (!heuristic_rule.empty()) {
        std::cerr << " (auto";
        if (explain) std::cerr << ", rule '" << heuristic_rule << "'";
        std::cerr << ")";
    }
    std::cerr << "\n";

    // Strategy selection: an explicit flag overrides the policy.
    std::string strategy_label = strategy_name_arg;
    std::string strategy_rule;

    if (strategy_label.empty()) {
        Decision d     = select(policy.strategy_rules, features);
        strategy_label = d.outcome;
        strategy_rule  = d.rule;
    }

    auto parsed = parse_strategy(strategy_label);
    if (!parsed) {
        std::cerr << "Error: unknown strategy '" << strategy_label << "'; expected one of:";
        for (auto& l : strategy_labels()) std::cerr << ' ' << l;
        std::cerr << "\n";
        return 1;
    }
    Strategy strategy = *parsed;

    if (!strategy_rule.empty()) {
        std::cerr << "[main] Strategy: " << strategy_name(strategy) << " (auto";
        if (explain) std::cerr << ", rule '" << strategy_rule << "'";
        std::cerr << ")\n";
    }

    using Clock = std::chrono::steady_clock;

    std::ofstream out(plan_path);
    if (!out.is_open()) {
        std::cerr << "Error: cannot open output file: " << plan_path << "\n";
        return 1;
    }

    if (strategy == Strategy::AOSTAR) {
        std::cerr << "[main] Mode: AO*\n";

        auto deadline = timeout_secs > 0
            ? Clock::now() + std::chrono::seconds(timeout_secs)
            : std::chrono::time_point<Clock>::max();

        auto t_start = Clock::now();

        auto result = aostar::search(task, *h, limit, deadline);

        if (!result) {
            // AO* exhausted its budget. For partial-plan-linear domains
            // (partial_obs=true, sensing=false) a conformant linear solution
            // may exist that AO* couldn't find within the time/depth budget.
            // GBFS with the remaining wall-clock budget has a different search
            // order and may succeed.
            if (!has_sensing_actions(task)) {
                std::cerr << "[main] AO* failed — falling back to GBFS\n";

                size_t remaining = 0;
                if (timeout_secs > 0) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                        Clock::now() - t_start).count();
                    remaining = (elapsed < (long long)timeout_secs)
                        ? timeout_secs - (size_t)elapsed : 0;
                }

                Deadline gbfs_deadline = remaining > 0
                    ? Clock::now() + std::chrono::seconds(remaining)
                    : Deadline::max();

                auto gbfs_result = gbfs::search(task, *h, limit, gbfs_deadline);
                if (gbfs_result) {
                    write_linear_plan(out, *gbfs_result);
                    std::cerr << "[main] Plan written to " << plan_path << "\n";
                    return 0;
                }
            }

            out << "null\n";
            std::cerr << "[main] No solution found.\n";
            return 0;
        }

        // An empty conditional plan means the goal already holds. Writing it as
        // "null" would make it indistinguishable from "no plan exists", which is
        // what the failure path emits; the empty array matches the convention
        // linear plans already use.
        if (!result->plan_tree) {
            out << "[]\n";
            std::cerr << "[main] Goal already satisfied — empty plan written to "
                      << plan_path << "\n";
        } else {
            write_plan_tree(out, result->plan_tree);
            out << "\n";
            std::cerr << "[main] Conditional plan written to " << plan_path << "\n";
        }

        auto vr = validate(task, result->plan_tree);
        if (vr.valid)
            std::cerr << "[validator] OK — " << vr.leaves_reached
                      << " leaves, " << vr.branches_checked << " branches checked\n";
        else
            std::cerr << "[validator] FAILED — " << vr.error << "\n";

    } else if (strategy == Strategy::EHC) {
        std::cerr << "[main] Mode: EHC\n";

        auto result = ehc::search(task, *h, limit);
        if (!result) {
            std::cerr << "[main] EHC failed — falling back to GBFS\n";
            result = gbfs::search(task, *h, limit);
        }

        if (!result) {
            out << "null\n";
            std::cerr << "[main] No solution found.\n";
            return 0;
        }

        write_linear_plan(out, *result);
        std::cerr << "[main] Plan written to " << plan_path << "\n";

    } else {
        std::cerr << "[main] Mode: GBFS\n";

        auto result = gbfs::search(task, *h, limit);
        if (!result) {
            out << "null\n";
            std::cerr << "[main] No solution found.\n";
            return 0;
        }

        write_linear_plan(out, *result);
        std::cerr << "[main] Plan written to " << plan_path << "\n";
    }

    return 0;
}