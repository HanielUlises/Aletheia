# Evaluation

Measured results for the implementation described in the [main README](../README.md).
Section references below (§n) point into that document.

Measured on the local benchmark suite, GCC 11.4, `-O3` with LTO, single core.
"Before" is the immediately preceding implementation; both binaries solve the
same 12 of 14 instances, and produce identical plans on every instance both
solve.

To reproduce any of these numbers, see [usage.md](usage.md).

## Wall-clock, mean of 20 runs

| instance | before | after | speedup |
|---|---:|---:|---:|
| `grapevine1` | 263.4 ms | 8.0 ms | **33.0×** |
| `coin4` | 92.2 ms | 8.6 ms | **10.7×** |
| `coin5` | 22.7 ms | 5.2 ms | 4.3× |
| `coin3` | 21.5 ms | 5.1 ms | 4.1× |
| `coin2` | 4.2 ms | 3.1 ms | 1.3× |
| `muddy3` | 2.4 ms | 1.8 ms | 1.3× |
| `coin1`, `muddy1`, `backdoor`, `sally-anne`, `whisper` | 1.8–3.4 ms | 1.6–3.4 ms | ≈1× |

Instances at the bottom of the table are dominated by process start-up (≈2 ms)
and measure nothing about the search.

## Largest solved instance

`gossip1` — 5 agents, 32 initial worlds, private announcements with
heterogeneous observability:

| | before | after |
|---|---:|---:|
| wall-clock | 151.23 s | **0.04 s** |
| peak resident memory | 21.0 GB | **7.3 MB** |

The memory figure is the representation change (§3.1) compounding with the
search change (§7): bit-matrix models are roughly two orders of magnitude
smaller than the hash-table representation, and the closed list now stores
16-byte fingerprints rather than whole models.

## AND-OR search

`amc1` is unsolvable, and both binaries fail on it — but for different reasons.
Deepening iterations completed in a fixed 20 s budget, which isolates the
persistent AO\* memo (§7.3) since the work per iteration is otherwise identical:

| before | after | ratio |
|---:|---:|---:|
| 13,902 | 275,104 | **19.8×** |

With the exhaustion proof of §9.1 the iteration count stops being the relevant
number: the instance now terminates at depth 1 in 3 ms with "no solution
exists", and its run log falls from 7,078 lines to 9. Read the 19.8× as a
measurement of the memo in isolation, not as current behaviour on `amc1`.

## Search effort

Canonical duplicate detection (§5) reduces expansions where symmetric states
occur:

| instance | expansions before | after |
|---|---:|---:|
| `coin4` | 715 | 629 |
| `coin3`, `coin5` | 243 | 221 |
| all others | — | unchanged |

The reduction is modest on this suite because the benchmark models are small
enough that few distinct labellings of the same situation arise. It is expected
to matter more on instances with many symmetric agents, where the previous
numbering-sensitive hash missed most duplicates.

## Heuristics across the suite

The heuristic comparison and the Gossip scaling argument live in §8.2 of the
main README, because they support a claim about where the planner's cost
actually sits rather than reporting a speedup. The short version: on Gossip
scaled from 5 to 7 agents every heuristic expands exactly the plan length, so
search cost grows two orders of magnitude while expansions stay optimal.

## Conditional plans on Consecutive Numbers

§9.2 tabulates cn-1 through cn-13 under both encodings of the announcement.
The result there is an encoding finding rather than a performance one, so it
stays with the argument it supports.
