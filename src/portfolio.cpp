#include "portfolio.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

namespace {

bool has_sensing(const PlanningTask& task) {
    for (const Action& a : task.actions)
        if (a.designated_events.size() > 1) return true;
    return false;
}

} // namespace

PortfolioOutcome race(const PlanningTask& task, const Heuristic& relaxation,
                      const Heuristic& spread, Deadline deadline) {
    using Run = std::function<void(PortfolioOutcome&)>;
    std::vector<std::pair<std::string, Run>> members = {
        {"gbfs+kadd", [&](PortfolioOutcome& o) { o.linear = gbfs::search(task, relaxation, 0, deadline); }},
        {"gbfs+ks",   [&](PortfolioOutcome& o) { o.linear = gbfs::search(task, spread, 0, deadline); }},
        // Exhaustion rules out policies only; linear plans can remain when
        // branching changes observability, so it does not end the race.
        {"aostar+kadd", [&](PortfolioOutcome& o) {
            o.contingent = aostar::search(task, relaxation, 0, deadline, &o.unsolvable);
        }},
    };
    if (has_sensing(task))
        members.push_back({"replan+kadd", [&](PortfolioOutcome& o) {
            o.contingent = replan::search(task, relaxation, deadline);
        }});

    std::atomic<bool> stop{false};
    std::mutex        m;
    PortfolioOutcome  winner;
    const auto        start = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (auto& [label, run] : members) {
        threads.emplace_back([&, label = label, run = run] {
            set_cancel_flag(&stop);
            PortfolioOutcome o;
            run(o);
            const bool found = o.linear.has_value() || o.contingent.has_value();
            const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            std::lock_guard lk(m);
            if (found && !stop.exchange(true)) {
                winner = std::move(o);
                winner.member = label;
                std::cerr << "[portfolio] " << label << " found a plan after " << secs << " s\n";
            } else if (!found) {
                std::cerr << "[portfolio] " << label << " stopped after " << secs << " s\n";
            }
        });
    }
    for (auto& t : threads) t.join();
    return winner;
}
