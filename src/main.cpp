#include "parser.hpp"
#include "validator.hpp"
#include "search.hpp"
#include "heuristic.hpp"

#include <iostream>
#include <fstream>
#include <string>
#include <stdexcept>
#include <memory>
#include <chrono>
#include <algorithm>

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

static int goal_modal_depth(const Formula& f) {
    switch (f.kind) {
        case FormulaKind::Belief:
        case FormulaKind::Common:
        case FormulaKind::Kw:
            return 1 + goal_modal_depth(*f.children[0]);
        case FormulaKind::And:
        case FormulaKind::Or: {
            int d = 0;
            for (auto& c : f.children)
                d = std::max(d, goal_modal_depth(*c));
            return d;
        }
        default:
            return 0;
    }
}

// select_heuristic — choose the best heuristic given task structure.
//
// The selection is based on three orthogonal task properties:
//
//   goal_kw_only:   every top-level goal conjunct is a Kw (knowing-whether)
//                   formula. KnowledgeSpreadHeuristic is purpose-built for
//                   this: it counts how many accessible worlds still fail to
//                   resolve each Kw conjunct, giving a tight gradient as
//                   knowledge propagates. EpistemicDistance wastes work here
//                   because it projects through Belief operators that the
//                   parser expands Kw into, double-counting uncertainty.
//
//   partial_obs:    some agents are Oblivious or have conditional observability.
//                   These domains rely on agents inferring what others know
//                   (or don't know) from the observability structure, not from
//                   direct belief propagation. KnowledgeSpread captures this
//                   better than EpistemicDistance because it measures the
//                   spread of knowing-whether across the agent graph, which is
//                   exactly what partial-obs actions manipulate.
//
//   sensing:        actions with |E_d| > 1. EpistemicDistance is good here
//                   because it gives a real-valued gradient toward resolving
//                   the branching uncertainty in the goal. UnsatisfiedGoal is
//                   too coarse (0/1 per conjunct) for sensing domains.
//
//   fallback:       UnsatisfiedGoal for purely ontic / shallow tasks where
//                   the goal has atom conjuncts — EpistemicDistance adds
//                   overhead without gradient benefit.
static std::unique_ptr<Heuristic> select_heuristic(const PlanningTask& task) {
    bool sensing = has_sensing_actions(task);

    if (task.goal_kw_only || task.partial_obs) {
        std::cerr << "[main] Heuristic: knowledge-spread (auto)\n";
        return std::make_unique<KnowledgeSpreadHeuristic>();
    }

    if (sensing || !has_atom_conjunct(*task.goal)) {
        std::cerr << "[main] Heuristic: epistemic-distance (auto)\n";
        return std::make_unique<EpistemicDistanceHeuristic>();
    }

    std::cerr << "[main] Heuristic: unsatisfied-goal (auto)\n";
    return std::make_unique<UnsatisfiedGoalHeuristic>();
}

// select_strategy — choose search algorithm given task structure.
//
// Priority order inside each branch is from most-constrained to least, so
// that a task matching multiple criteria gets the most specific strategy.
//
// Sensing branch (|E_d| > 1 in any action):
//   AO* is the only algorithm that correctly handles branching on sensing
//   outcomes. GBFS produces linear plans and cannot represent contingencies.
//   We prefer AO* whenever the state space is tractable: ed heuristic with
//   ≤16 designated worlds is the sweet spot from benchmarks.
//
// Partial-observability branch:
//   Gossip, Grapevine, and AMC have private announcements with heterogeneous
//   observability. These domains have a single designated world but a large
//   world set (32+) that grows ~1.5× per step — GBFS's world-count threshold
//   of 16 incorrectly sends them there. The plan is linear (no sensing
//   branches) but the search space is large; GBFS with KnowledgeSpread is
//   the right call. We route here before hitting the world-count threshold.
//
// KD45 belief with few worlds / designated:
//   AO* with KnowledgeSpread handles this well even with many ground actions
//   because the branching factor after bisim contraction is small.
//
// EHC:
//   Cheap and fast for shallow deterministic S5 tasks. Plateau-prone under
//   KD45 because BFS escape re-expands large belief models; restricted to
//   !kd45 domains.
//
// GBFS fallback:
//   Large world sets that don't fit the above cases.
static Strategy select_strategy(const PlanningTask& task) {
    bool sensing   = has_sensing_actions(task);
    int  depth     = goal_modal_depth(*task.goal);
    size_t desg    = task.init.designated.size();
    size_t worlds  = task.init.worlds.size();
    size_t actions = task.actions.size();

    if (sensing) {
        if (desg <= 16)
            return Strategy::AOSTAR;
        if (depth >= 2 && desg <= 32)
            return Strategy::AOSTAR;
        if (desg <= 8 && actions <= 32)
            return Strategy::AOSTAR;
        return Strategy::GBFS;
    }

    // Private-announcement domains with partial observability grow linearly
    // in worlds but have a linear plan structure. GBFS with KnowledgeSpread
    // works; AO* wastes time building contingent branches that never branch.
    if (task.partial_obs)
        return Strategy::GBFS;

    if (task.kd45 && worlds <= 8 && desg <= 4 && depth >= 1)
        return Strategy::AOSTAR;

    if (!task.kd45 && desg <= 4 && worlds <= 16 && actions <= 12 && depth <= 1)
        return Strategy::EHC;

    if (worlds > 16)
        return Strategy::GBFS;

    if (actions > 12)
        return Strategy::GBFS;

    return Strategy::EHC;
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
        << "  --heuristic    ug | ed | ks | wc  (default: auto)\n"
        << "  --limit        Max nodes / max depth (0 = unlimited)\n"
        << "  --timeout      Timeout in seconds (AO* only)\n"
        << "  --ehc          Force EHC\n"
        << "  --gbfs         Force GBFS\n"
        << "  --conditional  Force AO*\n"
        << "  --help         Show this message\n";
}

int main(int argc, char* argv[]) {

    std::string task_path;
    std::string plan_path;
    std::string heuristic_name;  // empty = auto

    size_t limit        = 0;
    size_t timeout_secs = 0;

    bool force_conditional = false;
    bool force_ehc         = false;
    bool force_gbfs        = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if      (arg == "--task"      && i+1 < argc) task_path      = argv[++i];
        else if (arg == "--plan"      && i+1 < argc) plan_path      = argv[++i];
        else if (arg == "--heuristic" && i+1 < argc) heuristic_name = argv[++i];
        else if (arg == "--limit"     && i+1 < argc) limit          = std::stoul(argv[++i]);
        else if (arg == "--timeout"   && i+1 < argc) timeout_secs   = std::stoul(argv[++i]);
        else if (arg == "--conditional") force_conditional = true;
        else if (arg == "--ehc")         force_ehc         = true;
        else if (arg == "--gbfs")        force_gbfs        = true;
        else if (arg == "--help" || arg == "-h") { usage(argv[0]); return 0; }
        else {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
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

    // Heuristic selection: explicit flag overrides auto-select.
    std::unique_ptr<Heuristic> h;
    if (!heuristic_name.empty()) {
        if      (heuristic_name == "ug") { h = std::make_unique<UnsatisfiedGoalHeuristic>();   std::cerr << "[main] Heuristic: unsatisfied-goal\n"; }
        else if (heuristic_name == "ed") { h = std::make_unique<EpistemicDistanceHeuristic>(); std::cerr << "[main] Heuristic: epistemic-distance\n"; }
        else if (heuristic_name == "ks") { h = std::make_unique<KnowledgeSpreadHeuristic>();   std::cerr << "[main] Heuristic: knowledge-spread\n"; }
        else                             { h = std::make_unique<WorldCountHeuristic>();         std::cerr << "[main] Heuristic: world-count\n"; }
    } else {
        h = select_heuristic(task);
    }

    // Strategy selection: explicit flag overrides auto-select.
    Strategy strategy;
    if      (force_conditional) strategy = Strategy::AOSTAR;
    else if (force_ehc)         strategy = Strategy::EHC;
    else if (force_gbfs)        strategy = Strategy::GBFS;
    else {
        strategy = select_strategy(task);
        std::cerr << "[main] Strategy: " << strategy_name(strategy) << " (auto)\n";
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

                auto gbfs_deadline = remaining > 0
                    ? Clock::now() + std::chrono::seconds(remaining)
                    : std::chrono::time_point<Clock>::max();

                (void)gbfs_deadline;  // gbfs::search takes node limit, not deadline
                auto gbfs_result = gbfs::search(task, *h, limit);
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

        write_plan_tree(out, result->plan_tree);
        out << "\n";
        std::cerr << "[main] Conditional plan written to " << plan_path << "\n";

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