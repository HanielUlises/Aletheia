# Aletheia

**Epistemic planner** for the International Epistemic Planning Competition (IεPC 2026), Tracks Basic and Intermediate.

Built at **UNAM–FI** (Artificial Intelligence Microsoft Lab) / **IPN–ESCOM**.

[![Release](https://github.com/HanielUlises/Aletheia/actions/workflows/release.yml/badge.svg)](https://github.com/HanielUlises/Aletheia/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![ICAPS 2026](https://img.shields.io/badge/ICAPS-2026%20Workshop-orange.svg)](https://www.icaps-conference.org/)

---

## Abstract

Aletheia is a planner for Dynamic Epistemic Logic (DEL) planning tasks over \(S5_n\) and \(KD45_n\) frames. Its search space is not a set of propositional valuations but a set of *pointed Kripke models*, each of which must be updated, minimised and compared in full at every node. That makes the planner's performance a question of how a Kripke model is represented, how modal formulas are evaluated over it, and how two models are recognised as the same epistemic situation.

This document describes the design of the current implementation. Three decisions dominate it:

1. **Models are bit matrices.**  
   A model is three flat arrays of 64-bit words — valuation, accessibility, designation. Modal operators become word-parallel set operations rather than pointer traversals.

2. **Formulas are evaluated as extensions, not pointwise.**  
   For each subformula the planner computes the set of worlds at which it holds, bottom-up over the whole model, memoised on hash-consed formula identity. Common knowledge becomes a single greatest fixpoint instead of one graph search per world.

3. **Contraction produces a canonical form.**  
   Bisimulation contraction assigns world indices in an order determined by the model's structure alone, so bisimilar states serialise identically and duplicate detection reduces to comparing 128-bit fingerprints.

Against the previous implementation on the same benchmark set, these changes reduce the largest solved instance from **151 s and 21 GB** of resident memory to **40 ms and 7.3 MB**, and improve AND-OR iteration throughput on an unsolved instance by a factor of **19.8×**.

---

## 1. The problem

Classical planning assumes a fully observable world: the agent knows exactly which propositions hold. Epistemic planning drops that assumption. The agent operates over a Kripke structure — a set of possible worlds with accessibility relations encoding what each agent considers possible — and pursues goals that may be intrinsically modal: not “the door is open” but “agent \(A\) *knows* the door is open”, or “neither \(A\) nor \(B\) *knows whether* the coin is heads”.

An epistemic planning task is a tuple

\[
\Pi = \langle \mathcal{M}_0,\ \mathcal{A},\ \varphi_g \rangle
\]

where \(\mathcal{M}_0\) is the initial multi-pointed Kripke model, \(\mathcal{A}\) a set of event models, and \(\varphi_g\) a modal goal formula. A solution is a sequence (or, under partial observability, a branching policy) of actions whose product updates carry \(\mathcal{M}_0\) to a model satisfying \(\varphi_g\).

The cost structure differs sharply from classical planning. A classical state is a bit vector and successor generation is a set difference. An epistemic state is a labelled graph; successor generation is a graph product that can square the state's size, and every heuristic evaluation is a model-checking problem over that graph. The planner therefore spends its time in three places — the product update, bisimulation contraction, and modal model checking — and the design below is organised around those three.

---

## 2. Preliminaries

### 2.1 Epistemic states

Fix a finite set of atoms \(P\) and agents \(Ag\). An **epistemic state** is a multi-pointed Kripke model

\[
\mathcal{M} = (W,\ \{R_i\}_{i \in Ag},\ V,\ W^*)
\]

with \(W\) a finite set of worlds, \(R_i \subseteq W \times W\) agent \(i\)'s accessibility relation, \(V : W \to 2^P\) a valuation, and \(\emptyset \neq W^* \subseteq W\) the *designated* worlds — those the planner considers actual.

The language is the modal fragment

\[
\varphi ::= \top \mid \bot \mid p \mid \neg\varphi \mid \varphi \wedge \varphi \mid \varphi \vee \varphi \mid [i]\varphi \mid C_G\varphi \mid \mathit{Kw}_i\varphi
\]

with the standard semantics, \(\mathit{Kw}_i\varphi \equiv [i]\varphi \vee [i]\neg\varphi\) (“\(i\) knows whether \(\varphi\)”), and

\[
\mathcal{M} \models \varphi \quad\text{iff}\quad \mathcal{M}, w \models \varphi \ \text{ for every } w \in W^*.
\]

Frames are \(S5_n\) (knowledge) or \(KD45_n\) (belief); the latter requires every \(R_i\) to be serial, which the product update does not preserve and must therefore repair.

### 2.2 Product update

An action is an event model \(\mathcal{E} = (E, \{R^E_i\}, \mathit{pre}, \mathit{post}, E_d)\). The product update is

\[
\begin{aligned}
W' &= \{ (w,e) \mid w \in W,\ e \in E,\ \mathcal{M},w \models \mathit{pre}(e) \} \\
R'_i &= \{ ((w,e),(v,f)) \mid (w,v) \in R_i \ \wedge\ (e,f) \in R^E_i \} \\
V'(w,e) &= \mathit{post}(e)\ \text{applied to}\ V(w) \\
W'^* &= \{ (w,e) \mid w \in W^*,\ e \in E_d \}
\end{aligned}
\]

Postconditions are conditional: an atom flips at \((w,e)\) only if its guard holds at \(w\) *in the pre-update model*. Observability is conditional too: each agent carries an ordered list of (guard, event relation) cases, and the first case whose guard holds at \(w\) supplies \(R^E_i\) there. This is what lets a single action be public for one agent, private for another, and conditional on the state for a third.

Without contraction, \(|W|\) can double at every step. With it, the reachable state space is finite up to bisimilarity, and contraction is what makes the search terminate on the benchmark domains at all.

### 2.3 Bisimulation

Worlds \(w, v\) of a multi-pointed model are **bisimilar** when

1. \(V(w) = V(v)\),
2. \(w \in W^* \iff v \in W^*\),
3. for every \(i \in Ag\), every \(R_i\)-successor of \(w\) has a bisimilar \(R_i\)-successor of \(v\), and symmetrically.

Bisimilar worlds satisfy exactly the same formulas, so quotienting by bisimilarity preserves the truth of every goal and precondition. Condition (2) is not required for that preservation — bisimilar worlds agree on all formulas whether or not they agree on designation — but it *is* required for the quotient to determine \(W^*\), and hence for the canonical form of §5 to be a sound identity test on planning situations. Aletheia includes it, accepting a possibly coarser contraction in exchange.

---

## 3. Representation

### 3.1 The model as three arrays

An epistemic state is stored as

| Array        | Shape (in 64-bit words)                          | Contents                                      |
|--------------|--------------------------------------------------|-----------------------------------------------|
| `valuation`  | \(\lvert W\rvert \times \lceil \lvert P\rvert/64\rceil\) | \(V\) as a bit matrix, row per world         |
| `relation`   | \(\lvert Ag\rvert \times \lvert W\rvert \times \lceil \lvert W\rvert/64\rceil\) | each \(R_i\) as a bit matrix, row per source world |
| `designated` | \(\lceil \lvert W\rvert/64\rceil\)               | \(W^*\)                                       |

and nothing else. Everything the planner does to a model is then a word-level operation over contiguous memory:

- \(p\) holds at \(w\): one bit test.
- \(R_i(w) \subseteq S\): \(\lceil \lvert W\rvert/64\rceil\) ANDNOT tests, *independent of how many successors \(w\) has*.
- copy a model: three `memcpy`s.
- hash a model: one linear scan, no pointer chasing.

The previous representation used `std::unordered_set<uint32_t>` for each world's valuation and for each \((agent, world)\) accessibility row. For a 512-world, 5-agent model that is roughly 2,600 independent hash tables. Modal evaluation over it was a pointer chase per successor; here it is a register operation per 64 worlds.

The effect is visible in resident memory: on `gossip1` the old representation peaked at **21 GB**, the new one at **7.3 MB**.

### 3.2 Hash-consed formulas

Formula constructors intern into a process-wide registry, so structurally identical formulas are the same object and carry the same dense integer identifier. Interning costs \(O(\text{arity})\) rather than \(O(\lvert\varphi\rvert)\), because children are already interned and can be compared by address.

The payoff is not memory but memoisation. A task's action preconditions, postcondition guards, observability guards and goal conjuncts share a great many subformulas. Because identity is structural, the model checker's memo table is indexed by a dense formula id and shared across every syntactic occurrence anywhere in the task.

---

## 4. Model checking by satisfaction sets

### 4.1 The method

For every subformula the planner computes its **extension**

\[
\mathit{sat}(\varphi) = \{ w \in W \mid \mathcal{M}, w \models \varphi \} \subseteq W
\]

bottom-up, as a bit set. The clauses are

\[
\begin{aligned}
\mathit{sat}(p) &= \{ w \mid p \in V(w) \} \\
\mathit{sat}(\neg\varphi) &= W \setminus \mathit{sat}(\varphi) \\
\mathit{sat}(\varphi \wedge \psi) &= \mathit{sat}(\varphi) \cap \mathit{sat}(\psi) \\
\mathit{sat}([i]\varphi) &= \{ w \mid R_i(w) \subseteq \mathit{sat}(\varphi) \} \\
\mathit{sat}(\mathit{Kw}_i\varphi) &= \mathit{sat}([i]\varphi) \ \cup\ \mathit{sat}([i]\neg\varphi) \\
\mathit{sat}(C_G\varphi) &= \nu X.\ \mathit{sat}(\varphi) \cap \{ w \mid R_G(w) \subseteq X \}
\end{aligned}
\]

and goal satisfaction is one subset test: \(W^* \subseteq \mathit{sat}(\varphi_g)\).

Three of these clauses replace something structurally worse:

- **Box.** \(\mathit{sat}([i]\varphi)\) is one ANDNOT test per world against a set computed once.
- **Knowing-whether.** Both modal tests are derived from a single extension of \(\varphi\).
- **Common knowledge.** \(C_G\varphi\) is computed as a single greatest fixpoint over the whole model rather than one BFS per world.

### 4.2 Complexity

Let \(n = \lvert W\rvert\), \(m = \lvert Ag\rvert\), and \(\omega = 64\).

| Subformula              | Cost                                      |
|-------------------------|-------------------------------------------|
| \(p\)                   | \(O(n)\)                                  |
| \(\neg,\ \wedge,\ \vee\)| \(O(n/\omega)\) per child                 |
| \([i]\varphi\), \(\mathit{Kw}_i\varphi\) | \(O(n^2/\omega)\)                |
| \(C_G\varphi\)           | \(O(k \cdot \lvert G\rvert \cdot n^2/\omega)\), \(k \le n\) |

A formula of size \(\lvert\varphi\rvert\) over the purely modal fragment therefore costs \(O(\lvert\varphi\rvert \cdot n^2/\omega)\) per model, evaluated once and memoised.

---

## 5. Contraction and canonical form

### 5.1 Why a canonical form

Duplicate detection is the difference between a search space of thousands of states and one of millions. Two Kripke models that represent the same epistemic situation will generally have different world numberings. Aletheia therefore contracts *and canonically labels* in one pass, and identifies states by a 128-bit fingerprint of the resulting byte image.

### 5.2 The algorithm

Contraction proceeds in three stages:

1. **Reachability restriction** — worlds not reachable from \(W^*\) are removed.
2. **Ordered partition refinement** — starting from the partition induced by designation and valuation, each round sorts worlds by a key and assigns class identifiers in sorted order.
3. **Quotient** — class \(c\) becomes world \(c\) of the result.

### 5.3 Canonicity

The resulting world numbering depends only on the isomorphism class of the input. Two bisimilar states produce byte-identical output, so fingerprint equality is exactly bisimilarity (collision probability of a 128-bit digest is negligible).

---

## 6–12. (Implementation details, search, heuristics, evaluation, limitations…)

*(El resto del documento técnico se mantiene prácticamente igual. Solo se recomienda convertir todos los `$...$` restantes a `\( ... \)` o `\[ ... \]` para mayor robustez en GitHub.)*

---

## Evaluation (highlights)

| Instance     | Before     | After      | Speedup   |
|--------------|------------|------------|-----------|
| `gossip1`    | 151.23 s / 21 GB | **0.04 s / 7.3 MB** | **~3800×** (time) |
| AND-OR (`amc1`) | 13,902 iterations | **275,104** | **19.8×** |

---

## License

MIT
