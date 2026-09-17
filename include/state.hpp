#pragma once
#include "bitset.hpp"
#include "formula.hpp"
#include "types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <span>

class SatCache;   // defined in state.cpp

// A 128-bit structural digest of a canonically-labelled epistemic state.
//
// Closed lists store fingerprints instead of states. A contracted state costs
// a few kilobytes; a fingerprint costs sixteen bytes, and comparison is two
// integer tests rather than a graph walk. At 128 bits the probability of a
// collision anywhere in a search of 10^9 states is below 10^-20, far below the
// probability of any other failure mode in the system.
struct Fingerprint {
    std::uint64_t lo{0};
    std::uint64_t hi{0};

    friend bool operator==(const Fingerprint&, const Fingerprint&) = default;
};

struct FingerprintHash {
    std::size_t operator()(const Fingerprint& f) const noexcept {
        return static_cast<std::size_t>(f.lo ^ (f.hi * 0x9E3779B97F4A7C15ULL));
    }
};

// Epistemic state = multi-pointed Kripke model (W, {R_i}_{i∈Ag}, V, W*).
//
// Storage:
//
//   valuation   num_worlds × val_words     V : W → 2^P as a bit matrix
//   set_of      num_agents × num_worlds    R_i(w) as an index into the set table
//   set_begin   num_sets + 1               offsets into members
//   members     Σ |set|                    successor sets, sorted world lists
//   designated  rel_words                  W* ⊆ W
//
// Accessibility is a table of successor sets, not a bit matrix. Epistemic
// models repeat successor sets heavily: on K45 frames (transitive and
// Euclidean, which covers S5 and KD45) two successor sets of one agent are
// equal or disjoint, the product update with K45 event relations and the
// bisimulation quotient both preserve the frame, and so an agent's sets hold
// at most |W| entries together. A bit matrix costs |Ag|·|W|² bits whatever the
// frame: 553 MB for one 19 210-world IεPC hard gossip state. Modal operators
// read the table once per set: [i]φ holds at w exactly when every member of
// R_i(w) is in sat(φ), which is decided once for each distinct set.
//
// Set ids carry no meaning. States produced by bisim_contract intern sets by
// content in canonical order, so equal contracted states are equal arrays.
struct EpistemicState {
    std::uint32_t num_worlds{0};
    std::uint32_t num_atoms{0};
    std::uint32_t num_agents{0};

    std::uint32_t val_words{0};   // = words_for(num_atoms)
    std::uint32_t rel_words{0};   // = words_for(num_worlds): one world set

    std::vector<bits::Word>    valuation;
    std::vector<std::uint32_t> set_of;
    std::vector<std::uint32_t> set_begin;
    std::vector<WorldIdx>      members;
    std::vector<bits::Word>    designated;

    // All five are defined out of line: SatCache is incomplete here, and
    // std::unique_ptr needs the complete type to destroy it.
    EpistemicState();

    // The satisfaction cache and the fingerprint are derived data. They are
    // rebuilt lazily rather than copied, so copy construction stays a plain
    // copy of the arrays.
    EpistemicState(const EpistemicState& o);
    EpistemicState& operator=(const EpistemicState& o);
    EpistemicState(EpistemicState&&) noexcept;
    EpistemicState& operator=(EpistemicState&&) noexcept;
    ~EpistemicState();

    // Every world starts with the empty successor set, which is set 0.
    void allocate(std::uint32_t worlds, std::uint32_t atoms, std::uint32_t agents);

    // Element access.
    [[nodiscard]] bits::WordSpan val(WorldIdx w) noexcept
        { return {valuation.data() + std::size_t(w) * val_words, val_words}; }
    [[nodiscard]] bits::ConstWordSpan val(WorldIdx w) const noexcept
        { return {valuation.data() + std::size_t(w) * val_words, val_words}; }

    [[nodiscard]] std::uint32_t num_sets() const noexcept
        { return static_cast<std::uint32_t>(set_begin.size() - 1); }
    [[nodiscard]] std::uint32_t succ_set(AgentIdx ag, WorldIdx w) const noexcept
        { return set_of[std::size_t(ag) * num_worlds + w]; }
    [[nodiscard]] std::span<const WorldIdx> set(std::uint32_t id) const noexcept
        { return {members.data() + set_begin[id], set_begin[id + 1] - set_begin[id]}; }
    [[nodiscard]] std::span<const WorldIdx> succ(AgentIdx ag, WorldIdx w) const noexcept
        { return set(succ_set(ag, w)); }

    [[nodiscard]] bits::WordSpan designated_bits() noexcept
        { return {designated.data(), rel_words}; }
    [[nodiscard]] bits::ConstWordSpan designated_bits() const noexcept
        { return {designated.data(), rel_words}; }

    [[nodiscard]] bool has_atom(WorldIdx w, AtomIdx a) const noexcept
        { return bits::test(val(w), a); }
    [[nodiscard]] bool is_designated(WorldIdx w) const noexcept
        { return bits::test(designated_bits(), w); }
    [[nodiscard]] std::size_t num_designated() const noexcept
        { return bits::count(designated_bits()); }

    // Mutators. These invalidate the derived caches; use them while building a
    // state, not on one that has already been evaluated.
    void set_atom(WorldIdx w, AtomIdx a)  { bits::set(val(w), a);            invalidate(); }
    void set_designated(WorldIdx w)       { bits::set(designated_bits(), w); invalidate(); }

    // Appends a set given as a sorted, duplicate-free world list and returns
    // its id. No deduplication, and no cache invalidation: callers building or
    // rewriting relations point worlds at the result through set_of.
    std::uint32_t add_set(std::span<const WorldIdx> sorted_members);

    // Model checking.
    //
    // sat(φ) is the extension of φ: the set of worlds at which φ holds,
    // computed bottom-up over the whole model and memoised by formula id.
    // The returned span is owned by this state's cache and remains valid until
    // the next mutation *or the next sat() call*, since the cache arena may
    // reallocate; callers holding several extensions at once must copy them.
    [[nodiscard]] bits::ConstWordSpan sat(const Formula& f) const;

    // Convenience wrapper that copies the extension out of the cache arena.
    void sat_copy(const Formula& f, std::vector<bits::Word>& out) const;

    [[nodiscard]] bool holds_at(const Formula& f, WorldIdx w) const;

    // M ⊨ φ, i.e. φ holds at every designated world: W* ⊆ sat(φ).
    [[nodiscard]] bool satisfies(const Formula& f) const;

    void invalidate() const noexcept;

    // Frees the satisfaction cache but keeps the fingerprint. For states that
    // wait in an open list: the cache is rebuilt on demand.
    void drop_cache() const noexcept;

    // Identity.
    //
    // These compare the *labelled* structure, set table included. They are
    // exact up to isomorphism only for states produced by bisim_contract,
    // which numbers worlds and sets canonically; every state the search stores
    // has been through it.
    [[nodiscard]] Fingerprint fingerprint() const;
    [[nodiscard]] std::size_t hash() const;
    [[nodiscard]] bool operator==(const EpistemicState& o) const noexcept;

    void print(const std::vector<std::string>& atom_names,
               const std::vector<std::string>& agent_names) const;

    // Bytes of model storage, excluding derived caches.
    [[nodiscard]] std::size_t footprint() const noexcept {
        return (valuation.size() + designated.size()) * sizeof(bits::Word) +
               (set_of.size() + set_begin.size()) * sizeof(std::uint32_t) +
               members.size() * sizeof(WorldIdx);
    }

private:
    mutable std::unique_ptr<SatCache>  cache_;
    mutable std::optional<Fingerprint> fp_;
};

// Interns successor sets by content while a relation is being built, so that
// equal sets share one id. Registers the empty set as id 0.
class SetInterner {
public:
    explicit SetInterner(EpistemicState& s);

    [[nodiscard]] std::uint32_t intern(std::span<const WorldIdx> sorted_members);

private:
    EpistemicState&                                s_;
    std::unordered_map<std::uint64_t, std::uint32_t> first_;   // content hash → id
    std::vector<std::uint32_t>                     next_;     // id → next id, same hash
};

// A state without its caches, for states that wait in a search queue. The set
// table already stores each distinct successor set once, so this is a plain
// copy of the arrays.
struct CompactState {
    std::uint32_t num_worlds{0}, num_atoms{0}, num_agents{0};
    std::vector<bits::Word>    valuation, designated;
    std::vector<std::uint32_t> set_of, set_begin;
    std::vector<WorldIdx>      members;

    [[nodiscard]] static CompactState from(const EpistemicState& s);
    [[nodiscard]] EpistemicState expand() const;
    [[nodiscard]] std::size_t footprint() const noexcept {
        return (valuation.size() + designated.size()) * sizeof(bits::Word) +
               (set_of.size() + set_begin.size()) * sizeof(std::uint32_t) +
               members.size() * sizeof(WorldIdx);
    }
};

// Restrict a state to `keep`, compacting world indices to 0..|keep|-1.
//
// Used by KD45 seriality repair (drop non-serial worlds) and by bisimulation
// contraction (drop worlds unreachable from W*). `remap` is filled with the
// old→new index of every retained world and kNoWorld elsewhere; callers holding
// world indices into the source — notably the product update's (w,e) table —
// patch them through it.
[[nodiscard]] EpistemicState restrict_state(const EpistemicState& s,
                                            bits::ConstWordSpan keep,
                                            std::vector<WorldIdx>& remap);
