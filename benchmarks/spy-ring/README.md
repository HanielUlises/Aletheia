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

| mode | solved | notes |
|---|---:|---|
| linear | 9/9 | GBFS + kadd; a9-k6-r7 (64 worlds): 21-step plan, 2.5 s |
| sensing | 9/9 | replan + kadd; a8-k6-r7: 64-leaf policy, 43 s, 97 MB |
