# Usage

Build, run, and configure the planner. For what it does and why, see the
[main README](../README.md); for measured results, see
[evaluation.md](evaluation.md).

## Requirements

- A C++23 compiler. GCC 11.4 is what the results were measured with.
- CMake ≥ 3.16
- [nlohmann/json](https://github.com/nlohmann/json) ≥ 3.10 (`nlohmann-json3-dev`)
- Python 3, for `serialize.py` and the benchmark scripts

## Build

```sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
```

The binary lands at `build/epistemic_planner`. Release builds enable `-O3` and
LTO, then strip; both matter for the figures in [evaluation.md](evaluation.md).

An Apptainer definition for the competition image is in `Apptainer.aletheia`.

## Running

```sh
build/epistemic_planner --task <task.json> --plan <plan.json> [options]
```

`--task` and `--plan` are required. The task is a grounded JSON task (the file
carrying a `"planning-task-info"` key). On success the plan file holds a JSON
array for a linear plan or a nested object for a conditional one; on failure it
holds `null`. An empty array means the goal already held.

### Options

| Option | Meaning |
|---|---|
| `--task <path>` | Grounded JSON task (required) |
| `--plan <path>` | Output plan file (required) |
| `--heuristic <label>` | `ug`, `ed`, `ks`, `wc`, `rpg`, `radd`. Default: chosen by policy |
| `--strategy <label>` | `gbfs`, `ehc`, `aostar`. Default: chosen by policy |
| `--policy <path>` | Selection-policy JSON; replaces the built-in rules |
| `--print-policy` | Write the effective policy to stdout and exit |
| `--explain` | Report the task features and which rule decided each auto-selection |
| `--limit <n>` | Max nodes (GBFS/EHC) or max depth (AO\*); 0 = unlimited |
| `--timeout <s>` | Timeout in seconds (AO\* only) |
| `--gbfs`, `--ehc`, `--conditional` | Aliases for `--strategy gbfs` / `ehc` / `aostar` |
| `--help` | Show usage |

An unknown heuristic or strategy label is an error listing the valid ones, not
a silent fallback.

### Wrapper scripts

- `./aletheia.sh <task.json> <plan.json>` — the IεPC 2026 entry point. Builds
  the binary if missing, runs it, then flattens any conditional plan tree to a
  flat array via `serialize.py`, which the competition output format requires.
  Note that it passes `--heuristic ed` by default, so it does **not** exercise
  automatic heuristic selection unless you override it.
- `./build_test.sh` — builds and runs the whole `benchmarks/` tree, writing one
  log per instance to `smoke-logs/`. Also defaults to `--heuristic ed`.
- `./run_benchmarks.sh` — sweeps a task set across several heuristics into
  `results/`.

## Selection policy

With neither `--heuristic` nor `--strategy` given, the planner picks both from
a rule table. The table is data, not code: it can be dumped, edited, and
supplied back without rebuilding.

```sh
build/epistemic_planner --print-policy > policy.json
# edit policy.json
build/epistemic_planner --task t.json --plan p.json --policy policy.json
```

### How rules are evaluated

Each of the two rule lists — `strategy` and `heuristic` — is evaluated
**first-match-wins**. A rule matches when *every* condition in its `when` array
holds. A rule with no conditions always matches, so it terminates the list and
acts as the default.

```json
{
  "strategy": [
    { "name": "sensing-deep-goal", "outcome": "aostar",
      "when": [
        { "feature": "sensing",          "op": "==", "value": 1 },
        { "feature": "goal_modal_depth", "op": ">=", "value": 2 },
        { "feature": "designated",       "op": "<=", "value": 32 }
      ] },
    { "name": "default", "outcome": "gbfs" }
  ]
}
```

A conjunction is one rule; a disjunction is two rules with the same `outcome`.
Operators are `<=`, `<`, `>=`, `>`, `==`, `!=`. Values are numbers — booleans
are `0` and `1`.

A file may supply `strategy`, `heuristic`, or both; whichever section is
omitted keeps its built-in rules.

### Features

Every quantity a condition can test, all read from the task before search
starts:

| Feature | Meaning |
|---|---|
| `sensing` | 1 if any action has more than one designated event |
| `max_designated_events` | max \|E_d\| over all actions |
| `worlds` | \|W\| in the initial state |
| `designated` | \|W\*\| in the initial state |
| `actions` | number of ground actions |
| `agents`, `atoms` | \|Ag\|, \|P\| |
| `goal_modal_depth` | deepest nesting of `[i]` / `C_G` / `Kw` in the goal |
| `goal_kw_only` | 1 if every top-level goal conjunct is a `Kw` formula |
| `goal_has_atom_conjunct` | 1 if the goal has a classical (non-modal) conjunct |
| `kd45` | 1 for a KD45 frame, 0 for S5 |
| `partial_obs` | 1 if any action has heterogeneous observability |

Outcomes must be `gbfs`, `ehc`, or `aostar` for strategy, and `ug`, `ed`, `ks`,
`wc`, `rpg`, `radd` for heuristic.

### Validation

A policy file is validated at load and the planner refuses to start if it does
not check out — an unknown feature or outcome, an unknown operator, a rule
shadowed by an earlier unconditional one, or a list with no terminal default.
Planning under a policy other than the one you asked for is worse than not
starting, so there is no fallback to the built-in table.

```
Error in selection policy: strategy rule 'typo' tests unknown feature 'wrlds';
expected one of: sensing max_designated_events worlds designated actions ...
```

### Seeing what fired

```sh
build/epistemic_planner --task benchmarks/coin4/problem_4.json \
                        --plan /dev/null --explain
```

```
[main] Features: sensing=1 max_designated_events=2 worlds=2 designated=1 ...
[main] Heuristic: knowledge-spread (auto, rule 'kw-only-goal')
[main] Strategy: AO* (auto, rule 'sensing-small-designated')
```

The thresholds in the built-in rules are tuned to the 15-instance suite in
`benchmarks/`. They are a starting point, not a claim about epistemic planning
tasks in general — retuning them for a different task distribution is the
reason the table is data.
