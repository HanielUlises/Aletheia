#include "state.hpp"

#include <unordered_map>

#include <algorithm>
#include <cassert>
#include <iostream>

// Satisfaction-set model checking
//
// Model checking proceeds by extension rather than by per-world descent. A
// per-world evaluator holds_at(φ, w) walks φ once for every world it is asked
// about, so [i]φ over |W| worlds re-descends into φ once per (source,
// successor) pair and C_G φ requires an independent breadth-first search from
// every world, with φ re-checked at every node visited; nested modalities
// multiply those factors.
//
// This evaluator instead computes, for each subformula, its extension
//
//     sat(φ) = { w ∈ W : M, w ⊨ φ }
//
// bottom-up, as a bit set over W. The modal cases become set operations:
//
//     sat(¬φ)     = W \ sat(φ)
//     sat(φ ∧ ψ)  = sat(φ) ∩ sat(ψ)
//     sat([i]φ)   = { w : R_i(w) ⊆ sat(φ) }           -- one test per set
//     sat(Kw_i φ) = sat([i]φ) ∪ sat([i]¬φ)            -- same pass, both ways
//     sat(C_G φ)  = νX. sat(φ) ∩ { w : R_G(w) ⊆ X }   -- one worklist, not one
//                                                        BFS per world
//
// Each subformula is evaluated exactly once per model, and because formulas are
// hash-consed the memo is shared across every occurrence of a subformula
// anywhere in the task: a precondition that also appears as an observability
// guard is computed once.
//

class SatCache {
public:
    explicit SatCache(const EpistemicState& s)
        : s_(s), nw_(s.num_worlds), rw_(s.rel_words) {}

    bits::ConstWordSpan sat(const Formula& f) { return cat(resolve(f)); }

private:
    const EpistemicState& s_;
    std::uint32_t nw_;
    std::uint32_t rw_;

    // One slot per computed extension. The outer vector may reallocate as slots
    // are added, but that only moves the inner vector *objects* — their heap
    // buffers stay put, so a span handed out by sat() remains valid for the
    // lifetime of the cache. Callers may therefore hold several extensions at
    // once, which the product update relies on.
    std::vector<std::vector<bits::Word>> slots_;
    std::vector<std::int64_t>            offset_;   // formula id → slot, -1 unknown

    [[nodiscard]] std::uint32_t alloc() {
        slots_.emplace_back(rw_, 0);
        return static_cast<std::uint32_t>(slots_.size() - 1);
    }

    [[nodiscard]] bits::WordSpan at(std::uint32_t i) noexcept {
        return {slots_[i].data(), rw_};
    }
    [[nodiscard]] bits::ConstWordSpan cat(std::uint32_t i) const noexcept {
        return {slots_[i].data(), rw_};
    }

    std::uint32_t resolve(const Formula& f) {
        if (offset_.size() <= f.id)
            offset_.resize(std::max<std::size_t>(f.id + 1, formula_universe_size()), -1);
        if (offset_[f.id] >= 0)
            return static_cast<std::uint32_t>(offset_[f.id]);

        const std::uint32_t out = compute(f);
        offset_[f.id] = static_cast<std::int64_t>(out);
        return out;
    }

    std::uint32_t compute(const Formula& f) {
        switch (f.kind) {
        case FormulaKind::Top: {
            const std::uint32_t out = alloc();
            bits::fill_all(at(out), nw_);
            return out;
        }

        case FormulaKind::Bot:
            return alloc();   // zero-initialised

        case FormulaKind::Atom: {
            // Column extraction: gather bit `atom` from every world's valuation.
            const std::uint32_t out = alloc();
            auto dst = at(out);
            for (WorldIdx w = 0; w < nw_; ++w)
                if (s_.has_atom(w, f.atom)) bits::set(dst, w);
            return out;
        }

        case FormulaKind::Not: {
            const std::uint32_t c   = resolve(*f.children[0]);
            const std::uint32_t out = alloc();
            bits::complement_into(at(out), cat(c), nw_);
            return out;
        }

        case FormulaKind::And: {
            std::vector<std::uint32_t> cs;
            cs.reserve(f.children.size());
            for (const auto& c : f.children) cs.push_back(resolve(*c));

            const std::uint32_t out = alloc();
            bits::fill_all(at(out), nw_);
            for (std::uint32_t c : cs) bits::and_into(at(out), cat(c));
            return out;
        }

        case FormulaKind::Or: {
            std::vector<std::uint32_t> cs;
            cs.reserve(f.children.size());
            for (const auto& c : f.children) cs.push_back(resolve(*c));

            const std::uint32_t out = alloc();
            for (std::uint32_t c : cs) bits::or_into(at(out), cat(c));
            return out;
        }

        case FormulaKind::Belief: {
            const std::uint32_t c   = resolve(*f.children[0]);
            const std::uint32_t out = alloc();
            if (f.agent < s_.num_agents) box_into(at(out), cat(c), f.agent);
            return out;
        }

        case FormulaKind::Kw: {
            // [i]φ ∨ [i]¬φ from a single extension of φ: a set qualifies when
            // its members all agree on φ.
            const std::uint32_t c   = resolve(*f.children[0]);
            const std::uint32_t out = alloc();
            if (f.agent < s_.num_agents) kw_into(at(out), cat(c), f.agent);
            return out;
        }

        case FormulaKind::Common: {
            // Greatest fixpoint of X ↦ sat(φ) ∩ { w : ∀i∈G. R_i(w) ⊆ X }.
            //
            // w belongs to the fixpoint exactly when φ holds at every world
            // reachable from w by the reflexive-transitive closure of ⋃_{i∈G} R_i.
            // Its complement is the least fixpoint of
            //
            //     Y ↦ ¬sat(φ) ∪ { w : ∃i∈G. R_i(w) ∩ Y ≠ ∅ },
            //
            // computed backwards from the ¬φ worlds: a world entering Y marks
            // every G-set containing it, and a set marked once adds every world
            // pointing at it. Each set and each (agent, world) pair is visited
            // once.
            const std::uint32_t c   = resolve(*f.children[0]);
            const std::uint32_t out = alloc();
            common_into(at(out), cat(c), f.group);
            return out;
        }
        }
        return alloc();   // unreachable
    }

    // Per-set flags, computed only for the sets agent `ag` points at.
    struct Flags {
        std::vector<std::uint32_t> stamp;
        std::vector<std::uint8_t>  in, out;   // all members in src / none in src
        std::uint32_t              token{0};
    };

    static Flags& flags(std::uint32_t num_sets) {
        thread_local Flags fl;
        if (fl.stamp.size() < num_sets) {
            fl.stamp.assign(num_sets, 0);
            fl.in.resize(num_sets);
            fl.out.resize(num_sets);
            fl.token = 0;
        }
        if (++fl.token == 0) {
            std::fill(fl.stamp.begin(), fl.stamp.end(), 0);
            fl.token = 1;
        }
        return fl;
    }

    void classify(Flags& fl, std::uint32_t id, bits::ConstWordSpan src) const {
        fl.stamp[id] = fl.token;
        bool all = true, none = true;
        for (WorldIdx v : s_.set(id)) {
            if (bits::test(src, v)) none = false;
            else                    all  = false;
            if (!all && !none) break;
        }
        fl.in[id]  = all;
        fl.out[id] = none;
    }

    // dst := { w : R_ag(w) ⊆ src }
    void box_into(bits::WordSpan dst, bits::ConstWordSpan src, AgentIdx ag) const {
        Flags& fl = flags(s_.num_sets());
        for (WorldIdx w = 0; w < nw_; ++w) {
            const std::uint32_t id = s_.succ_set(ag, w);
            if (fl.stamp[id] != fl.token) classify(fl, id, src);
            if (fl.in[id]) bits::set(dst, w);
        }
    }

    // dst := { w : R_ag(w) ⊆ src or R_ag(w) ∩ src = ∅ }
    void kw_into(bits::WordSpan dst, bits::ConstWordSpan src, AgentIdx ag) const {
        Flags& fl = flags(s_.num_sets());
        for (WorldIdx w = 0; w < nw_; ++w) {
            const std::uint32_t id = s_.succ_set(ag, w);
            if (fl.stamp[id] != fl.token) classify(fl, id, src);
            if (fl.in[id] || fl.out[id]) bits::set(dst, w);
        }
    }

    void common_into(bits::WordSpan dst, bits::ConstWordSpan src,
                     const std::vector<AgentIdx>& group) const {
        const std::uint32_t ns = s_.num_sets();

        thread_local std::vector<AgentIdx>      agents;
        thread_local std::vector<std::uint32_t> user_begin, users, cont_begin, cont, fill;
        thread_local std::vector<std::uint8_t>  used, hit;
        thread_local std::vector<WorldIdx>      queue;
        thread_local std::vector<bits::Word>    bad;

        agents.clear();
        for (AgentIdx ag : group)
            if (ag < s_.num_agents) agents.push_back(ag);

        // users(S): worlds pointing at S through a G-agent.
        user_begin.assign(std::size_t(ns) + 1, 0);
        used.assign(ns, 0);
        for (AgentIdx ag : agents)
            for (WorldIdx w = 0; w < nw_; ++w) {
                const std::uint32_t id = s_.succ_set(ag, w);
                ++user_begin[id + 1];
                used[id] = 1;
            }
        for (std::uint32_t i = 0; i < ns; ++i) user_begin[i + 1] += user_begin[i];
        users.resize(user_begin[ns]);
        fill.assign(user_begin.begin(), user_begin.end() - 1);
        for (AgentIdx ag : agents)
            for (WorldIdx w = 0; w < nw_; ++w) users[fill[s_.succ_set(ag, w)]++] = w;

        // containing(v): G-sets with v as a member.
        cont_begin.assign(std::size_t(nw_) + 1, 0);
        for (std::uint32_t id = 0; id < ns; ++id)
            if (used[id])
                for (WorldIdx v : s_.set(id)) ++cont_begin[v + 1];
        for (WorldIdx v = 0; v < nw_; ++v) cont_begin[v + 1] += cont_begin[v];
        cont.resize(cont_begin[nw_]);
        fill.assign(cont_begin.begin(), cont_begin.end() - 1);
        for (std::uint32_t id = 0; id < ns; ++id)
            if (used[id])
                for (WorldIdx v : s_.set(id)) cont[fill[v]++] = id;

        bad.assign(rw_, 0);
        bits::complement_into(bad, src, nw_);
        queue.clear();
        bits::for_each(bad, [&](std::uint32_t w) { queue.push_back(w); });
        hit.assign(ns, 0);
        for (std::size_t q = 0; q < queue.size(); ++q) {
            const WorldIdx y = queue[q];
            for (std::uint32_t k = cont_begin[y]; k < cont_begin[y + 1]; ++k) {
                const std::uint32_t id = cont[k];
                if (hit[id]) continue;
                hit[id] = 1;
                for (std::uint32_t u = user_begin[id]; u < user_begin[id + 1]; ++u) {
                    const WorldIdx w = users[u];
                    if (bits::test(bad, w)) continue;
                    bits::set(bad, w);
                    queue.push_back(w);
                }
            }
        }
        bits::complement_into(dst, bad, nw_);
    }
};

//
// EpistemicState
//

EpistemicState::EpistemicState() = default;

EpistemicState::EpistemicState(const EpistemicState& o)
    : num_worlds(o.num_worlds), num_atoms(o.num_atoms), num_agents(o.num_agents),
      val_words(o.val_words), rel_words(o.rel_words),
      valuation(o.valuation), set_of(o.set_of), set_begin(o.set_begin),
      members(o.members), designated(o.designated), fp_(o.fp_) {}

EpistemicState& EpistemicState::operator=(const EpistemicState& o) {
    if (this == &o) return *this;
    num_worlds = o.num_worlds; num_atoms = o.num_atoms; num_agents = o.num_agents;
    val_words  = o.val_words;  rel_words = o.rel_words;
    valuation  = o.valuation;  set_of    = o.set_of;    set_begin  = o.set_begin;
    members    = o.members;    designated = o.designated;
    cache_.reset();
    fp_ = o.fp_;
    return *this;
}

// A SatCache holds a back-reference to the state it was built from, so it can
// never be carried across a move — the moved-from object's address is what it
// captured. Both moves therefore drop the cache on each side and let it be
// rebuilt lazily. The fingerprint has no such dependency and is carried over.
EpistemicState::EpistemicState(EpistemicState&& o) noexcept
    : num_worlds(o.num_worlds), num_atoms(o.num_atoms), num_agents(o.num_agents),
      val_words(o.val_words), rel_words(o.rel_words),
      valuation(std::move(o.valuation)), set_of(std::move(o.set_of)),
      set_begin(std::move(o.set_begin)), members(std::move(o.members)),
      designated(std::move(o.designated)), fp_(o.fp_) {
    o.cache_.reset();
}

EpistemicState& EpistemicState::operator=(EpistemicState&& o) noexcept {
    if (this == &o) return *this;
    num_worlds = o.num_worlds; num_atoms = o.num_atoms; num_agents = o.num_agents;
    val_words  = o.val_words;  rel_words = o.rel_words;
    valuation  = std::move(o.valuation);
    set_of     = std::move(o.set_of);
    set_begin  = std::move(o.set_begin);
    members    = std::move(o.members);
    designated = std::move(o.designated);
    cache_.reset();
    o.cache_.reset();
    fp_ = o.fp_;
    return *this;
}

EpistemicState::~EpistemicState() = default;

void EpistemicState::allocate(std::uint32_t worlds, std::uint32_t atoms,
                              std::uint32_t agents) {
    num_worlds = worlds;
    num_atoms  = atoms;
    num_agents = agents;
    val_words  = static_cast<std::uint32_t>(bits::words_for(atoms));
    rel_words  = static_cast<std::uint32_t>(bits::words_for(worlds));

    valuation.assign(std::size_t(worlds) * val_words, 0);
    set_of.assign(std::size_t(agents) * worlds, 0);
    set_begin.assign(2, 0);
    members.clear();
    designated.assign(rel_words, 0);
    invalidate();
}

std::uint32_t EpistemicState::add_set(std::span<const WorldIdx> sorted_members) {
    members.insert(members.end(), sorted_members.begin(), sorted_members.end());
    set_begin.push_back(static_cast<std::uint32_t>(members.size()));
    return static_cast<std::uint32_t>(set_begin.size() - 2);
}

namespace {

std::uint64_t content_hash(std::span<const WorldIdx> xs) noexcept {
    std::uint64_t h = bits::mix64(0x9E3779B97F4A7C15ULL ^ xs.size());
    std::size_t i = 0;
    for (; i + 1 < xs.size(); i += 2)
        h = bits::mix64(h ^ ((std::uint64_t(xs[i]) << 32) | xs[i + 1]));
    if (i < xs.size()) h = bits::mix64(h ^ xs[i]);
    return h;
}

} // namespace

SetInterner::SetInterner(EpistemicState& s) : s_(s) {
    for (std::uint32_t id = 0; id < s_.num_sets(); ++id) {
        next_.push_back(UINT32_MAX);
        auto [it, fresh] = first_.try_emplace(content_hash(s_.set(id)), id);
        if (!fresh) {
            std::uint32_t k = it->second;
            while (next_[k] != UINT32_MAX) k = next_[k];
            next_[k] = id;
        }
    }
}

std::uint32_t SetInterner::intern(std::span<const WorldIdx> sorted_members) {
    const std::uint64_t h = content_hash(sorted_members);
    auto it = first_.find(h);
    std::uint32_t last = UINT32_MAX;
    if (it != first_.end()) {
        for (std::uint32_t k = it->second; k != UINT32_MAX; k = next_[k]) {
            const auto m = s_.set(k);
            if (std::equal(m.begin(), m.end(), sorted_members.begin(), sorted_members.end()))
                return k;
            last = k;
        }
    }
    const std::uint32_t id = s_.add_set(sorted_members);
    next_.push_back(UINT32_MAX);
    if (last == UINT32_MAX) first_.emplace(h, id);
    else                    next_[last] = id;
    return id;
}

void EpistemicState::invalidate() const noexcept {
    cache_.reset();
    fp_.reset();
}

void EpistemicState::drop_cache() const noexcept { cache_.reset(); }

bits::ConstWordSpan EpistemicState::sat(const Formula& f) const {
    if (!cache_) cache_ = std::make_unique<SatCache>(*this);
    return cache_->sat(f);
}

void EpistemicState::sat_copy(const Formula& f, std::vector<bits::Word>& out) const {
    const auto s = sat(f);
    out.assign(s.begin(), s.end());
}

bool EpistemicState::holds_at(const Formula& f, WorldIdx w) const {
    assert(w < num_worlds);
    return bits::test(sat(f), w);
}

bool EpistemicState::satisfies(const Formula& f) const {
    if (num_worlds == 0) return true;   // vacuous: W* is empty
    return bits::subset_of(designated_bits(), sat(f));
}

// Identity.

Fingerprint EpistemicState::fingerprint() const {
    if (fp_) return *fp_;

    // Two independent 64-bit streams over the same word sequence, each seeded
    // and finalised with splitmix64, so one flipped bit anywhere in the model
    // changes roughly half the bits of both halves.
    bits::Word h1 = bits::mix64(0x243F6A8885A308D3ULL ^ num_worlds);
    bits::Word h2 = bits::mix64(0x13198A2E03707344ULL ^
                                ((std::uint64_t(num_agents) << 32) | num_atoms));

    const auto absorb = [&](bits::Word w) noexcept {
        h1 = bits::mix64(h1 ^ w);
        h2 = bits::mix64((h2 + w) * 0x9E3779B97F4A7C15ULL);
    };

    const auto absorb32 = [&](const std::vector<std::uint32_t>& xs) noexcept {
        std::size_t i = 0;
        for (; i + 1 < xs.size(); i += 2) absorb((std::uint64_t(xs[i]) << 32) | xs[i + 1]);
        if (i < xs.size()) absorb(xs[i]);
        absorb(xs.size());
    };

    for (bits::Word w : valuation)  absorb(w);
    absorb(0xA5A5A5A5A5A5A5A5ULL);          // domain separator between arrays
    absorb32(set_of);
    absorb32(set_begin);
    absorb32(members);
    absorb(0x5A5A5A5A5A5A5A5AULL);
    for (bits::Word w : designated) absorb(w);

    fp_ = Fingerprint{h1, h2};
    return *fp_;
}

std::size_t EpistemicState::hash() const {
    return static_cast<std::size_t>(fingerprint().lo);
}

bool EpistemicState::operator==(const EpistemicState& o) const noexcept {
    return num_worlds == o.num_worlds && num_atoms == o.num_atoms &&
           num_agents == o.num_agents &&
           valuation  == o.valuation  && set_of == o.set_of &&
           set_begin  == o.set_begin  && members == o.members &&
           designated == o.designated;
}

// Restriction.

CompactState CompactState::from(const EpistemicState& s) {
    CompactState c;
    c.num_worlds = s.num_worlds;
    c.num_atoms  = s.num_atoms;
    c.num_agents = s.num_agents;
    c.valuation  = s.valuation;
    c.designated = s.designated;
    c.set_of     = s.set_of;
    c.set_begin  = s.set_begin;
    c.members    = s.members;
    return c;
}

EpistemicState CompactState::expand() const {
    EpistemicState s;
    s.num_worlds = num_worlds;
    s.num_atoms  = num_atoms;
    s.num_agents = num_agents;
    s.val_words  = static_cast<std::uint32_t>(bits::words_for(num_atoms));
    s.rel_words  = static_cast<std::uint32_t>(bits::words_for(num_worlds));
    s.valuation  = valuation;
    s.designated = designated;
    s.set_of     = set_of;
    s.set_begin  = set_begin;
    s.members    = members;
    s.invalidate();
    return s;
}

EpistemicState restrict_state(const EpistemicState& s,
                              bits::ConstWordSpan keep,
                              std::vector<WorldIdx>& remap) {
    remap.assign(s.num_worlds, kNoWorld);

    std::vector<WorldIdx> survivors;
    survivors.reserve(bits::count(keep));
    bits::for_each(keep, [&](std::uint32_t w) {
        remap[w] = static_cast<WorldIdx>(survivors.size());
        survivors.push_back(w);
    });

    EpistemicState out;
    const auto nw = static_cast<std::uint32_t>(survivors.size());
    out.allocate(nw, s.num_atoms, s.num_agents);

    for (std::size_t w = 0; w < survivors.size(); ++w)
        bits::copy_from(out.val(static_cast<WorldIdx>(w)), s.val(survivors[w]));

    // Remapping is monotone, so filtered sets stay sorted. A set is rewritten
    // once however many worlds point at it.
    std::vector<std::uint32_t> new_id(s.num_sets(), UINT32_MAX);
    std::vector<WorldIdx> buf;
    for (AgentIdx ag = 0; ag < s.num_agents; ++ag)
        for (WorldIdx w = 0; w < nw; ++w) {
            const std::uint32_t id = s.succ_set(ag, survivors[w]);
            if (new_id[id] == UINT32_MAX) {
                buf.clear();
                for (WorldIdx v : s.set(id))
                    if (remap[v] != kNoWorld) buf.push_back(remap[v]);
                new_id[id] = buf.empty() ? 0 : out.add_set(buf);
            }
            out.set_of[std::size_t(ag) * nw + w] = new_id[id];
        }

    auto des = out.designated_bits();
    bits::for_each(s.designated_bits(), [&](std::uint32_t w) {
        if (remap[w] != kNoWorld) bits::set(des, remap[w]);
    });

    out.invalidate();
    return out;
}

// Debug output.

void EpistemicState::print(const std::vector<std::string>& atom_names,
                           const std::vector<std::string>& agent_names) const {
    std::cout << "Worlds: " << num_worlds << "  Designated: {";
    bits::for_each(designated_bits(), [&](std::uint32_t w) { std::cout << w << " "; });
    std::cout << "}\n";

    for (WorldIdx w = 0; w < num_worlds; ++w) {
        std::cout << "  w" << w;
        if (is_designated(w)) std::cout << "*";
        std::cout << ": {";
        for (AtomIdx a = 0; a < num_atoms; ++a) {
            if (!has_atom(w, a)) continue;
            if (a < atom_names.size()) std::cout << atom_names[a] << " ";
            else                       std::cout << a << " ";
        }
        std::cout << "}\n";
    }

    for (AgentIdx ag = 0; ag < num_agents; ++ag) {
        const std::string aname =
            (ag < agent_names.size()) ? agent_names[ag] : std::to_string(ag);
        std::cout << "  R_" << aname << ": ";
        for (WorldIdx w = 0; w < num_worlds; ++w)
            for (WorldIdx v : succ(ag, w)) std::cout << w << "->" << v << " ";
        std::cout << "\n";
    }
}
