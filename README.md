# Aletheia

Epistemic planner for the International Epistemic Planning Competition (IεPC 2026), Track Basic and Intermediate.
Built at IPN–ESCOM / UNAM–FFyL.

[![Release](https://github.com/HanielUlises/Aletheia/actions/workflows/release.yml/badge.svg)](https://github.com/HanielUlises/Aletheia/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
---

## The problem

Classical planning assumes a fully observable world: the agent knows exactly which propositions hold. Epistemic planning drops that assumption. The agent operates over a *Kripke structure* — a set of possible worlds with accessibility relations encoding what each agent considers possible — and must act to achieve goals that may be intrinsically modal: not just "the door is open" but "agent A *knows* the door is open" or "neither A nor B *knows* whether the coin is heads."

Formally, an epistemic planning task is a tuple

$$\langle \mathcal{M}_0,\ \mathcal{A},\ \varphi_g \rangle$$

where $\mathcal{M}_0 = (W, R, V, D)$ is the initial multi-pointed Kripke model, $\mathcal{A}$ is a set of event models drawn from Dynamic Epistemic Logic, and $\varphi_g$ is a modal goal formula. Executing action $a \in \mathcal{A}$ produces a new model via the DEL *product update*:

$$\mathcal{M} \otimes \mathcal{E}_a = (W \times E,\ R',\ V',\ D')$$

restricted to worlds satisfying the event preconditions and contracted under bisimulation to keep the state space tractable. Search proceeds over equivalence classes of Kripke models rather than propositional states — exponentially larger in the worst case but reducible through bisimulation contraction.

---

## Planner

Single self-contained C++17 binary. Input is a grounded JSON task from [plank](https://github.com/a-burigana/plank); output is a JSON plan.

Supports S5ₙ and KD45ₙ frames, public announcements, semi-private sensing, private ontic actions, knowing-whether (`Kw`) formulas, common knowledge goals, and conditional AND-OR planning.

Strategy selection is automatic, driven by an estimated complexity score:

$$\rho = \bar{b}^{\,d} \cdot |W| \cdot |Ag| \cdot \delta_{\text{KD45}}$$

where $\delta_{\text{KD45}} < 1$ discounts KD45 tasks for seriality-based reduction. This routes to AO\* for conditional sensing tasks, EHC for shallow deterministic problems, and GBFS for large effectively-linear spaces. Four heuristics are available (`ug`, `ed`, `ks`, `wc`); `ed` is the default.

---

## Usage

Download the latest binary from [Releases](../../releases) and run:

```bash
./aletheia.sh task.json plan.json
```

Or via Apptainer:

```bash
apptainer run Apptainer.epistemic_planner task.json plan.json
```

Optional flags: `--heuristic {ug|ed|ks|wc}`, `--timeout N`, `--limit N`, `--conditional`, `--ehc`, `--gbfs`.

Linear plans are emitted as JSON arrays. Conditional plans are AND-OR trees flattened to IPC-compatible arrays by the wrapper. Failure returns `null`.

---

## License

MIT
