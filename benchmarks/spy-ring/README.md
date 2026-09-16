# spy-ring

Friendly spies in a corridor of rooms must share secret keys while a mole
listens. Scalable in agents, keys, rooms and goal depth.

## Actions

| action | type | observability |
|---|---|---|
| `move ?i ?from ?to` | public-ontic | everyone |
| `decode ?i ?k` | quasi-private-sensing | same room partial, rest oblivious (sensing mode) |
| `tell ?i ?j ?k` | quasi-private sensing / announcement | same room **hears**, adjacent room sees, rest oblivious |

`tell` requires speaker and listener in the same room. Anyone else in that room
overhears the value, including the mole.

## Goal

- every friend knows whether each key holds;
- the mole knows none;
- depth 2: every friend knows the mole knows none.

## Modes

- `linear`: keys true, each holder knows its key. Plans are sequences;
  `plank validate` checks them.
- `sensing`: nobody knows any key; holders must `decode`. Plans branch on
  every key, up to 2^keys leaves.

## Usage

```sh
python3 generate.py --agents 5 --keys 3 --rooms 4 --mode sensing --depth 2
python3 generate.py --suite --out .     # standard suite into linear/, sensing/
./ground.sh                             # EPDDL -> JSON with plank
```

Grounded JSON over 1 MB is not committed; `ground.sh` rebuilds it.

## Aletheia, 60 s limit

| instance | worlds | result |
|---|---:|---|
| linear a5-k3-r4 | 8 | AO\*, 2.6M expansions, 53 s |
| linear a6-k3-r4 | 8 | timeout (AO\*) |
| linear a8-k5-r6 | 32 | GBFS, 286-step plan, 1.1 s |
| sensing a4-k2-r3 | 4 | AO\*, 466k expansions, 6.7 s |
| sensing a5-k2-r3 and up | 4–64 | timeout; 6–7 GB at a7–a8 |
