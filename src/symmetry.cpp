#include "symmetry.hpp"

#include "bisimulation.hpp"

#include <algorithm>
#include <numeric>
#include <optional>
#include <string>
#include <unordered_map>

namespace {

std::vector<std::string> split(const std::string& name) {
    std::vector<std::string> out(1);
    for (char c : name) {
        if (c == '_') out.emplace_back();
        else          out.back().push_back(c);
    }
    return out;
}

// Renames every token equal to agent a or b, swapping them. Nullopt if the name
// mentions neither.
std::optional<std::string> swap_tokens(const std::string& name,
                                       const std::string& a, const std::string& b) {
    auto tokens = split(name);
    bool touched = false;
    for (auto& t : tokens) {
        if (t == a)      { t = b; touched = true; }
        else if (t == b) { t = a; touched = true; }
    }
    if (!touched) return std::nullopt;
    std::string out = tokens[0];
    for (std::size_t i = 1; i < tokens.size(); ++i) out += '_' + tokens[i];
    return out;
}

// Structural hash invariant under reordering of ∧ / ∨ children, after renaming.
std::uint64_t hash(const Formula& f, const AgentSymmetry::Swap* s) {
    const auto ag = [&](AgentIdx x) {
        return !s ? x : x == s->a ? s->b : x == s->b ? s->a : x;
    };
    std::uint64_t h = bits::mix64(static_cast<std::uint64_t>(f.kind) + 1);
    switch (f.kind) {
        case FormulaKind::Atom:
            return bits::mix64(h ^ (s ? s->atom[f.atom] : f.atom));
        case FormulaKind::Belief:
        case FormulaKind::Kw:
            h = bits::mix64(h ^ ag(f.agent));
            return bits::mix64(h ^ hash(*f.children[0], s));
        case FormulaKind::Common: {
            std::uint64_t g = 0;
            for (AgentIdx x : f.group) g += bits::mix64(ag(x) + 0x51);
            h = bits::mix64(h ^ g);
            return bits::mix64(h ^ hash(*f.children[0], s));
        }
        case FormulaKind::Not:
            return bits::mix64(h ^ hash(*f.children[0], s));
        case FormulaKind::And:
        case FormulaKind::Or: {
            std::uint64_t sum = 0;
            for (auto& c : f.children) sum += hash(*c, s);
            return bits::mix64(h ^ sum);
        }
        default:
            return h;
    }
}

// Exact test that renaming f by s yields g, up to reordering of ∧ / ∨.
bool equal(const Formula& f, const AgentSymmetry::Swap& s, const Formula& g) {
    if (f.kind != g.kind) return false;
    const auto ag = [&](AgentIdx x) { return x == s.a ? s.b : x == s.b ? s.a : x; };
    switch (f.kind) {
        case FormulaKind::Top:
        case FormulaKind::Bot:
            return true;
        case FormulaKind::Atom:
            return s.atom[f.atom] == g.atom;
        case FormulaKind::Not:
            return equal(*f.children[0], s, *g.children[0]);
        case FormulaKind::Belief:
        case FormulaKind::Kw:
            return ag(f.agent) == g.agent && equal(*f.children[0], s, *g.children[0]);
        case FormulaKind::Common: {
            std::vector<AgentIdx> x;
            for (AgentIdx a : f.group) x.push_back(ag(a));
            std::vector<AgentIdx> y = g.group;
            std::sort(x.begin(), x.end());
            std::sort(y.begin(), y.end());
            return x == y && equal(*f.children[0], s, *g.children[0]);
        }
        case FormulaKind::And:
        case FormulaKind::Or: {
            if (f.children.size() != g.children.size()) return false;
            // Pair children by hash; a collision can only cause a false negative.
            const std::size_t n = f.children.size();
            std::vector<std::pair<std::uint64_t, const Formula*>> fc(n), gc(n);
            for (std::size_t i = 0; i < n; ++i) {
                fc[i] = {hash(*f.children[i], &s), f.children[i].get()};
                gc[i] = {hash(*g.children[i], nullptr), g.children[i].get()};
            }
            std::sort(fc.begin(), fc.end());
            std::sort(gc.begin(), gc.end());
            for (std::size_t i = 0; i < n; ++i)
                if (fc[i].first != gc[i].first || !equal(*fc[i].second, s, *gc[i].second))
                    return false;
            return true;
        }
    }
    return false;
}

bool equal_posts(const std::unordered_map<AtomIdx, FormulaPtr>& x,
                 const AgentSymmetry::Swap& s,
                 const std::unordered_map<AtomIdx, FormulaPtr>& y) {
    if (x.size() != y.size()) return false;
    for (const auto& [atom, cond] : x) {
        auto it = y.find(s.atom[atom]);
        if (it == y.end() || !equal(*cond, s, *it->second)) return false;
    }
    return true;
}

bool maps_action(const Action& x, const AgentSymmetry::Swap& s, const Action& y) {
    if (x.events.size() != y.events.size() ||
        x.designated_events != y.designated_events)
        return false;

    for (std::size_t e = 0; e < x.events.size(); ++e) {
        const Event& ex = x.events[e];
        const Event& ey = y.events[e];
        if (!equal(*ex.precondition, s, *ey.precondition) ||
            !equal_posts(ex.post_true, s, ey.post_true) ||
            !equal_posts(ex.post_false, s, ey.post_false))
            return false;
    }

    const std::size_t na = std::max(x.obs_cases.size(), y.obs_cases.size());
    static const std::vector<ObsCase> none;
    for (AgentIdx i = 0; i < na; ++i) {
        const AgentIdx j = i == s.a ? s.b : i == s.b ? s.a : i;
        const auto& cx = i < x.obs_cases.size() ? x.obs_cases[i] : none;
        const auto& cy = j < y.obs_cases.size() ? y.obs_cases[j] : none;
        if (cx.size() != cy.size()) return false;
        for (std::size_t c = 0; c < cx.size(); ++c)
            if (cx[c].relation_bits != cy[c].relation_bits ||
                !equal(*cx[c].condition, s, *cy[c].condition))
                return false;
    }
    return true;
}

// The state with agents a ↔ b and their atoms swapped.
EpistemicState rename(const EpistemicState& s, const AgentSymmetry::Swap& sw) {
    EpistemicState out;
    out.allocate(s.num_worlds, s.num_atoms, s.num_agents);

    for (WorldIdx w = 0; w < s.num_worlds; ++w) {
        auto dst = out.val(w);
        bits::for_each(s.val(w), [&](std::uint32_t p) { bits::set(dst, sw.atom[p]); });
    }
    for (AgentIdx ag = 0; ag < s.num_agents; ++ag) {
        const AgentIdx to = ag == sw.a ? sw.b : ag == sw.b ? sw.a : ag;
        for (WorldIdx w = 0; w < s.num_worlds; ++w)
            bits::copy_from(out.succ(to, w), s.succ(ag, w));
    }
    bits::copy_from(out.designated_bits(), s.designated_bits());
    out.invalidate();
    return out;
}

} // namespace

AgentSymmetry AgentSymmetry::detect(const PlanningTask& task) {
    AgentSymmetry sym;
    const std::size_t na = task.num_agents();

    sym.action_agents.resize(task.num_actions());
    for (ActionIdx x = 0; x < task.num_actions(); ++x)
        for (const auto& t : split(task.actions[x].name)) {
            auto it = task.agent_index.find(t);
            if (it == task.agent_index.end()) continue;
            auto& v = sym.action_agents[x];
            if (std::find(v.begin(), v.end(), it->second) == v.end())
                v.push_back(it->second);
        }

    const Fingerprint init_fp = bisim_contract(task.init).fingerprint();

    for (AgentIdx a = 0; a < na; ++a) {
        for (AgentIdx b = a + 1; b < na; ++b) {
            const auto& an = task.agent_names[a];
            const auto& bn = task.agent_names[b];

            Swap s{a, b, {}, {}};
            bool ok = true;

            s.atom.resize(task.num_atoms());
            for (AtomIdx p = 0; p < task.num_atoms() && ok; ++p) {
                s.atom[p] = p;
                if (auto r = swap_tokens(task.atom_names[p], an, bn)) {
                    auto it = task.atom_index.find(*r);
                    if (it == task.atom_index.end()) ok = false;
                    else                             s.atom[p] = it->second;
                }
            }

            s.action.resize(task.num_actions());
            for (ActionIdx x = 0; x < task.num_actions() && ok; ++x) {
                s.action[x] = x;
                if (auto r = swap_tokens(task.actions[x].name, an, bn)) {
                    auto it = task.action_index.find(*r);
                    if (it == task.action_index.end()) ok = false;
                    else                               s.action[x] = it->second;
                }
            }

            ok = ok && (!task.goal || equal(*task.goal, s, *task.goal));
            for (ActionIdx x = 0; x < task.num_actions() && ok; ++x)
                ok = maps_action(task.actions[x], s, task.actions[s.action[x]]);
            ok = ok && bisim_contract(rename(task.init, s)).fingerprint() == init_fp;

            if (ok) sym.swaps.push_back(std::move(s));
        }
    }
    return sym;
}

StateSymmetry stabiliser(const AgentSymmetry& sym, const EpistemicState& s) {
    StateSymmetry st;
    const std::uint32_t na = s.num_agents;

    std::vector<std::uint32_t> parent(na);
    std::iota(parent.begin(), parent.end(), 0);
    const auto find = [&](std::uint32_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };

    // Cheap necessary conditions before the canonical-form test.
    std::vector<std::size_t> edges(na, 0);
    for (AgentIdx ag = 0; ag < na; ++ag)
        for (WorldIdx w = 0; w < s.num_worlds; ++w)
            edges[ag] += bits::count(s.succ(ag, w));

    const Fingerprint fp = s.fingerprint();
    for (const auto& sw : sym.swaps) {
        if (find(sw.a) == find(sw.b) || edges[sw.a] != edges[sw.b]) continue;
        if (bisim_contract(rename(s, sw)).fingerprint() != fp) continue;
        parent[find(sw.a)] = find(sw.b);
        st.trivial = false;
    }
    if (st.trivial) return st;

    // Number components by smallest member, list members ascending.
    st.component.assign(na, UINT32_MAX);
    std::vector<std::uint32_t> id_of_root(na, UINT32_MAX);
    std::uint32_t n = 0;
    for (AgentIdx ag = 0; ag < na; ++ag) {
        const std::uint32_t r = find(ag);
        if (id_of_root[r] == UINT32_MAX) id_of_root[r] = n++;
        st.component[ag] = id_of_root[r];
    }
    st.begin.assign(n + 1, 0);
    for (AgentIdx ag = 0; ag < na; ++ag) ++st.begin[st.component[ag] + 1];
    for (std::uint32_t c = 0; c < n; ++c) st.begin[c + 1] += st.begin[c];
    st.members.resize(na);
    std::vector<std::uint32_t> fill(st.begin.begin(), st.begin.end() - 1);
    for (AgentIdx ag = 0; ag < na; ++ag) st.members[fill[st.component[ag]]++] = ag;
    return st;
}

bool StateSymmetry::keep(const AgentSymmetry& sym, ActionIdx action) const {
    if (trivial) return true;
    thread_local std::vector<std::uint32_t> used;
    used.assign(begin.size(), 0);
    for (AgentIdx ag : sym.action_agents[action]) {
        const std::uint32_t c = component[ag];
        if (begin[c + 1] - begin[c] < 2) continue;
        if (members[begin[c] + used[c]++] != ag) return false;
    }
    return true;
}
