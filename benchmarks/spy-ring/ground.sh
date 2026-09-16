#!/usr/bin/env bash
# Grounds every spy-ring problem to JSON. Needs plank and the IεPC intermediate library.
#   PLANK=path/to/plank LIB=path/to/intermediate.epddl ./ground.sh
set -euo pipefail
cd "$(dirname "$0")"
PLANK=${PLANK:-plank}
LIB=${LIB:?set LIB to intermediate.epddl}
for mode in linear sensing; do
    for p in "$mode"/spy-*.epddl; do
        "$PLANK" export -d "$mode/domain.epddl" -p "$p" -l "$LIB" -o "$mode" >/dev/null
        echo "$p"
    done
done
