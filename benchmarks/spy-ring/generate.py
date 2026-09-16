#!/usr/bin/env python3
"""spy-ring: scalable epistemic benchmark generator.

A ring of spies in a corridor of rooms shares secret keys while a mole listens.

  move     public-ontic            agents walk between adjacent rooms
  decode   quasi-private-sensing   a codebook holder learns a key (sensing mode only)
  tell     same room hears, next room sees, rest oblivious
           (quasi-private-sensing in sensing mode, -announcement in linear mode)

Goals: every friend knows every key, the mole knows none, and (depth 2) every
friend knows the mole knows none.

Axes: --agents (incl. mole), --keys (2^keys worlds), --rooms, --mode, --depth.
"""
import argparse
import string
from pathlib import Path

DOMAIN = """(define (domain spy-ring)
    (:requirements
        :typing :equality :ontic-actions :conditional-effects :partial-observability
        :facts :lists :list-comprehensions :knowing-whether
        :general-preconditions :negative-postconditions :negative-list-formulas
        :quantified-obs-conditions :negative-obs-conditions
    )

    (:action-type-libraries intermediate)

    (:types room key)

    (:predicates
        (at ?i - agent ?r - room)
        (secret ?k - key)
        (:fact adjacent ?r1 ?r2 - room)
        (:fact codebook ?i - agent ?k - key)
        (:fact mole ?i - agent)
    )

    (:event nil)

    ;-------------------- MOVE --------------------

    (:event e-move
        :parameters (?i - agent ?from ?to - room)
        :precondition (at ?i ?from)
        :effects (:and (not (at ?i ?from)) (at ?i ?to))
    )

    (:action move
        :parameters (?i - agent ?from ?to - room | (adjacent ?from ?to))
        :action-type (public-ontic (e-move ?i ?from ?to))
        :observability-conditions (default Fully)
    )

{decode}    ;-------------------- TELL --------------------

    (:event e-tell-pos
        :parameters (?i ?j - agent ?k - key)
        :precondition (and
            (secret ?k)
            ([?i] (secret ?k))
            (exists (?r - room) (and (at ?i ?r) (at ?j ?r))))
    )

    (:event e-tell-neg
        :parameters (?i ?j - agent ?k - key)
        :precondition (and
            (not (secret ?k))
            ([?i] (not (secret ?k)))
            (exists (?r - room) (and (at ?i ?r) (at ?j ?r))))
    )

    (:action tell
        :parameters (?i ?j - agent ?k - key | (and (/= ?i ?j) (not (mole ?i))))
        :action-type ({tell_type} (e-tell-pos ?i ?j ?k) (e-tell-neg ?i ?j ?k) (nil))
        :observability-conditions
            (:and
                (?i Fully)
                (?j Fully)
                (:forall (?l - agent | (and (/= ?l ?i) (/= ?l ?j)))
                    (?l
                        (if (exists (?r - room) (and (at ?i ?r) (at ?l ?r)))
                            Fully
                        else-if (exists (?r ?s - room) (and (at ?i ?r) (at ?l ?s) (adjacent ?r ?s)))
                            Partially
                        else
                            Oblivious ))))
    )
)
"""


DECODE = """    ;-------------------- DECODE --------------------

    (:event e-decode-pos
        :parameters (?i - agent ?k - key)
        :precondition (secret ?k)
    )

    (:event e-decode-neg
        :parameters (?i - agent ?k - key)
        :precondition (not (secret ?k))
    )

    (:action decode
        :parameters (?i - agent ?k - key | (codebook ?i ?k))
        :action-type (quasi-private-sensing (e-decode-pos ?i ?k) (e-decode-neg ?i ?k) (nil))
        :observability-conditions
            (:and
                (?i Fully)
                (:forall (?j - agent | (/= ?i ?j))
                    (?j
                        (if (exists (?r - room) (and (at ?i ?r) (at ?j ?r)))
                            Partially
                        else
                            Oblivious ))))
    )

"""


def domain(mode: str) -> str:
    if mode == "sensing":
        return DOMAIN.format(decode=DECODE, tell_type="quasi-private-sensing")
    return DOMAIN.format(decode="", tell_type="quasi-private-announcement")


def problem(agents: int, keys: int, rooms: int, mode: str, depth: int) -> str:
    names = list(string.ascii_uppercase[:agents - 1]) + ["M"]
    friends, mole = names[:-1], "M"
    room_names = [f"r{r}" for r in range(1, rooms + 1)]
    key_names = [f"k{k}" for k in range(1, keys + 1)]

    # Codebooks round-robin over friends; agents spread along the corridor,
    # the mole in the middle room.
    codebook = {k: friends[i % len(friends)] for i, k in enumerate(key_names)}
    position = {a: room_names[(i * rooms) // len(friends)] for i, a in enumerate(friends)}
    position[mole] = room_names[rooms // 2]

    facts = [f"(adjacent {a} {b}) (adjacent {b} {a})"
             for a, b in zip(room_names, room_names[1:])]
    facts += [f"(codebook {h} {k})" for k, h in codebook.items()]
    facts.append(f"(mole {mole})")

    at = [f"(at {a} {position[a]})" for a in names]
    not_at = [f"(not (at {a} {r}))" for a in names for r in room_names if r != position[a]]
    init = at + [f"([C. All] (and {' '.join(at + not_at)}))"]
    if mode == "linear":
        # Keys are true and each holder already knows its key.
        init += [f"(secret {k})" for k in key_names]
        init += [f"([C. All] ([Kw. {h}] (secret {k})))" for k, h in codebook.items()]

    goal = [f"([Kw. {f}] (secret {k}))" for f in friends for k in key_names]
    goal += [f"(not ([Kw. {mole}] (secret {k})))" for k in key_names]
    if depth >= 2:
        goal += [f"([{f}] (not ([Kw. {mole}] (secret {k}))))"
                 for f in friends for k in key_names]

    name = f"spy-{mode}-a{agents}-k{keys}-r{rooms}-d{depth}"
    nl = "\n            "
    return f"""(define (problem {name})
    (:domain spy-ring)
    (:requirements :typing :equality :facts :finitary-S5-theories :modal-goals :negative-goals)

    (:agents {' '.join(names)})
    (:objects {' '.join(room_names)} - room {' '.join(key_names)} - key)

    (:facts-init
        {' '.join(facts)}
    )

    (:init
        (:and
            {nl.join(init)}
        )
    )

    (:goal
        (and
            {nl.join(goal)}
        )
    )
)
"""


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--agents", type=int, default=4, help="agents, including the mole (>= 3)")
    ap.add_argument("--keys", type=int, default=2)
    ap.add_argument("--rooms", type=int, default=3)
    ap.add_argument("--mode", choices=["sensing", "linear"], default="sensing")
    ap.add_argument("--depth", type=int, choices=[1, 2], default=2)
    ap.add_argument("--suite", action="store_true", help="write the standard suite")
    ap.add_argument("--out", type=Path, default=Path("."))
    args = ap.parse_args()

    configs = SUITE if args.suite else [(args.mode, args.agents, args.keys, args.rooms, args.depth)]
    for mode, agents, keys, rooms, depth in configs:
        if agents < 3 or rooms < 2 or keys < 1:
            ap.error("need agents >= 3, rooms >= 2, keys >= 1")
        out = args.out / mode if args.suite else args.out
        out.mkdir(parents=True, exist_ok=True)
        (out / "domain.epddl" if args.suite else out / f"domain-{mode}.epddl").write_text(domain(mode))
        p = problem(agents, keys, rooms, mode, depth)
        name = p.split("(problem ", 1)[1].split(")", 1)[0]
        (out / f"{name}.epddl").write_text(p)
        print(out / f"{name}.epddl")


# (mode, agents, keys, rooms, depth), roughly increasing difficulty per mode.
SUITE = [
    ("linear", 3, 1, 2, 2), ("linear", 4, 2, 3, 2), ("linear", 5, 2, 3, 2),
    ("linear", 5, 3, 4, 2), ("linear", 6, 3, 4, 2), ("linear", 6, 4, 5, 2),
    ("linear", 7, 4, 5, 2), ("linear", 8, 5, 6, 2), ("linear", 9, 6, 7, 2),
    ("sensing", 3, 1, 2, 2), ("sensing", 4, 1, 3, 2), ("sensing", 4, 2, 3, 2),
    ("sensing", 5, 2, 3, 2), ("sensing", 5, 3, 4, 2), ("sensing", 6, 3, 4, 2),
    ("sensing", 6, 4, 5, 2), ("sensing", 7, 5, 6, 2), ("sensing", 8, 6, 7, 2),
]


if __name__ == "__main__":
    main()
