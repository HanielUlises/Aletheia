#!/usr/bin/env python3
"""
flatten_plan.py — converts Aletheia's conditional plan tree to a flat JSON array.

The IεPC requires plans as flat JSON arrays: ["act_1", "act_2", ..., "act_k"]
Aletheia's AO* produces a conditional plan tree. This script flattens it by
following the first branch at each sensing node (the branch corresponding to
event 0 in the designated world).

Usage:
    python3 flatten_plan.py <plan.json>
    python3 flatten_plan.py <plan.json> --output flat_plan.json
    python3 flatten_plan.py <plan.json> --in-place   (overwrites the input file)

Returns:
    0 if a plan was found and flattened
    1 if the plan was null (no solution)
"""

import json
import sys
import argparse


def flatten(node):
    """Recursively flatten a conditional plan tree to a list of action names.
    Follows the first branch (event index 0) at each sensing node."""
    if node is None:
        return []

    actions = [node["action"]]

    branches = node.get("branches", [])
    if branches:
        first_branch = branches[0]
        subtree = first_branch.get("subtree")
        actions.extend(flatten(subtree))

    return actions


def main():
    parser = argparse.ArgumentParser(
        description="Flatten a conditional plan tree to a flat JSON array."
    )
    parser.add_argument("plan", help="Path to plan JSON file")
    parser.add_argument("--output", "-o", help="Output file (default: stdout)")
    parser.add_argument("--in-place", "-i", action="store_true",
                        help="Overwrite the input file with the flattened plan")
    args = parser.parse_args()

    with open(args.plan) as f:
        data = json.load(f)

    if data is None:
        result = "null\n"
        if args.in_place:
            with open(args.plan, "w") as f:
                f.write(result)
        elif args.output:
            with open(args.output, "w") as f:
                f.write(result)
        else:
            sys.stdout.write(result)
        sys.exit(1)

    if isinstance(data, list):
        result = json.dumps(data) + "\n"
        if args.in_place:
            with open(args.plan, "w") as f:
                f.write(result)
        elif args.output:
            with open(args.output, "w") as f:
                f.write(result)
        else:
            sys.stdout.write(result)
        sys.exit(0)

    flat = flatten(data)
    result = json.dumps(flat) + "\n"

    if args.in_place:
        with open(args.plan, "w") as f:
            f.write(result)
    elif args.output:
        with open(args.output, "w") as f:
            f.write(result)
    else:
        sys.stdout.write(result)

    sys.exit(0)


if __name__ == "__main__":
    main()
