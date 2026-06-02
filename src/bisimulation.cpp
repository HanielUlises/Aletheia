#include "bisimulation.hpp"
#include <algorithm>
#include <numeric>
#include <unordered_map>
#include <functional>

// Bisimulation contraction via partition refinement.
//
// Two worlds w, v are bisimilar iff:
//   1. V(w) = V(v)
//   2. For every agent i and every R_i-successor u of w there exists
//      an R_i-successor u' of v in the same bisimulation class, and vice-versa.
//
// The original implementation used std::map<std::vector<int>, int> to assign
// class ids inside the refinement loop. On dense models (gossip: 32 worlds,
// 512 pairs/agent) the ordered-map key comparison is O(na * nw) per lookup,
// making the whole loop O(nw^2 * na * log nw) per iteration and O(nw^3 * na)
// total enough to block on states of 256+ worlds before contraction even
// reduces anything.
//
// This version uses an unordered_map keyed on a uint64_t hash of the signature,
// falling back to a collision bucket for the rare case of hash collision.
// Refinement cost drops to O(nw * na) per iteration amortised.
//
// The correctness argument is identical to the original: we start with the
// atom-valuation partition and repeatedly split classes whose members disagree
// on the multiset of neighbour-classes for some agent, until stable.

static size_t hash_signature(int base_class,
                              const std::vector<std::vector<int>>& nbr_classes) {
    size_t h = std::hash<int>{}(base_class);
    for (auto& vec : nbr_classes) {
        for (int c : vec)
            h ^= std::hash<int>{}(c) + 0x9e3779b9u + (h << 6) + (h >> 2);
        h ^= 0xdeadbeef + (h << 6) + (h >> 2);
    }
    return h;
}

EpistemicState bisim_contract(EpistemicState s) {
    size_t nw = s.worlds.size();
    size_t na = s.num_agents;

    if (nw == 0) return s;

    std::vector<int> class_of(nw);

    // Initial partition by atom valuation.
    {
        std::unordered_map<size_t, int> seen;

        for (WorldIdx w = 0; w < nw; w++) {
            std::vector<AtomIdx> sig(s.worlds[w].atoms.begin(),
                                     s.worlds[w].atoms.end());
            std::sort(sig.begin(), sig.end());
            size_t h = 0;
            for (AtomIdx a : sig)
                h ^= std::hash<uint32_t>{}(a) + 0x9e3779b9u + (h << 6) + (h >> 2);
            auto [it, inserted] = seen.emplace(h, (int)seen.size());
            // Two worlds with the same hash but different atoms get the same
            // initial class, refinement will split them.
            class_of[w] = it->second;
        }
    }

    bool changed = true;
    while (changed) {
        changed = false;

        // For each world, build a signature:
        //   (current_class, [sorted_unique_neighbour_classes_for_agent_0], ..., [for_agent_na-1])
        // and assign a new class id via an unordered_map on the hash.
        //
        // Collision handling. signatures with the same hash go into a bucket;
        // within a bucket we do a full vector comparison to separate true
        // collisions from hash equality.

        using Sig = std::pair<size_t, std::vector<std::vector<int>>>;
        std::unordered_map<size_t, std::vector<std::pair<Sig, int>>> buckets;
        std::vector<int> new_class(nw);
        int next_id = 0;

        for (WorldIdx w = 0; w < nw; w++) {
            std::vector<std::vector<int>> nbr(na);
            for (AgentIdx ag = 0; ag < na; ag++) {
                for (WorldIdx v : s.accessibility[ag][w])
                    nbr[ag].push_back(class_of[v]);
                std::sort(nbr[ag].begin(), nbr[ag].end());
                nbr[ag].erase(std::unique(nbr[ag].begin(), nbr[ag].end()),
                               nbr[ag].end());
            }

            size_t h = hash_signature(class_of[w], nbr);
            Sig sig{h, std::move(nbr)};

            auto& bucket = buckets[h];
            int assigned = -1;
            for (auto& [existing_sig, id] : bucket) {
                if (existing_sig.first  == sig.first &&
                    existing_sig.second == sig.second) {
                    assigned = id;
                    break;
                }
            }
            if (assigned == -1) {
                assigned = next_id++;
                bucket.emplace_back(sig, assigned);
            }
            new_class[w] = assigned;
        }

        if (new_class != class_of) {
            class_of = new_class;
            changed  = true;
        }
    }

    int num_classes = *std::max_element(class_of.begin(), class_of.end()) + 1;
    std::vector<WorldIdx> repr(num_classes, static_cast<WorldIdx>(nw));
    for (WorldIdx w = 0; w < nw; w++) {
        int c = class_of[w];
        if (repr[c] == nw) repr[c] = w;
    }

    EpistemicState result;
    result.num_agents = na;

    for (int c = 0; c < num_classes; c++) {
        WorldIdx rw = repr[c];
        World w;
        w.id    = static_cast<WorldIdx>(c);
        w.atoms = s.worlds[rw].atoms;
        result.worlds.push_back(std::move(w));
    }

    for (WorldIdx w : s.designated)
        result.designated.insert(static_cast<WorldIdx>(class_of[w]));

    result.accessibility.resize(na, Relation(num_classes));
    for (AgentIdx ag = 0; ag < na; ag++) {
        for (int c = 0; c < num_classes; c++) {
            WorldIdx rw = repr[c];
            for (WorldIdx v : s.accessibility[ag][rw])
                result.accessibility[ag][c].insert(
                    static_cast<WorldIdx>(class_of[v]));
        }
    }

    return result;
}