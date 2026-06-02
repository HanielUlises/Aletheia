#!/usr/bin/env bash
# aletheia.sh — Aletheia epistemic planner wrapper
# IεPC 2026 competition entry point
#
# Usage:
#   ./aletheia.sh <task.json> <plan.json> [--heuristic ed|ug|wc|ks] [--timeout 60]
#
# This script:
#   1. Builds the planner if the binary is missing
#   2. Runs the epistemic_planner binary
#   3. Flattens any conditional plan tree to a flat JSON array
#      (required by IεPC output format policy)
#   4. Writes the result to <plan.json>

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PLANNER="$SCRIPT_DIR/build/epistemic_planner"
FLATTEN="$SCRIPT_DIR/serialize.py"

if [[ ! -x "$PLANNER" ]]; then
    echo "[aletheia] Planner binary not found — building now..." >&2
    cmake -B "$SCRIPT_DIR/build" -S "$SCRIPT_DIR" 2>&1 | sed 's/^/[cmake] /' >&2
    cmake --build "$SCRIPT_DIR/build" -j"$(nproc)" 2>&1 | sed 's/^/[build] /' >&2
    if [[ ! -x "$PLANNER" ]]; then
        echo "[aletheia] Error: build succeeded but binary still not found at $PLANNER" >&2
        exit 1
    fi
    echo "[aletheia] Build complete." >&2
fi

if [[ ! -f "$FLATTEN" ]]; then
    echo "[aletheia] Error: serialize.py not found at $FLATTEN" >&2
    exit 1
fi

TASK=""
PLAN=""
HEURISTIC="ed"
TIMEOUT=120
EXTRA_ARGS=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --heuristic) HEURISTIC="$2"; shift 2 ;;
        --timeout)   TIMEOUT="$2";   shift 2 ;;
        --conditional|--gbfs|--ehc)
                     EXTRA_ARGS="$EXTRA_ARGS $1"; shift ;;
        --limit)     EXTRA_ARGS="$EXTRA_ARGS $1 $2"; shift 2 ;;
        -*)
            echo "[aletheia] Unknown option: $1" >&2
            exit 1
            ;;
        *)
            if [[ -z "$TASK" ]]; then
                TASK="$1"
            elif [[ -z "$PLAN" ]]; then
                PLAN="$1"
            else
                echo "[aletheia] Unexpected argument: $1" >&2
                exit 1
            fi
            shift
            ;;
    esac
done

if [[ -z "$TASK" || -z "$PLAN" ]]; then
    echo "Usage: $0 <task.json> <plan.json> [--heuristic ed|ug|wc|ks] [--timeout N]" >&2
    exit 1
fi

if [[ ! -f "$TASK" ]]; then
    echo "[aletheia] Error: task file not found: $TASK" >&2
    exit 1
fi

echo "[aletheia] Task:      $TASK" >&2
echo "[aletheia] Plan:      $PLAN" >&2
echo "[aletheia] Heuristic: $HEURISTIC" >&2
echo "[aletheia] Timeout:   ${TIMEOUT}s" >&2

"$PLANNER" \
    --task      "$TASK" \
    --plan      "$PLAN" \
    --heuristic "$HEURISTIC" \
    --timeout   "$TIMEOUT" \
    $EXTRA_ARGS \
    2>&1 || true


if [[ -f "$PLAN" ]]; then
    python3 "$FLATTEN" "$PLAN" --in-place
    echo "[aletheia] Plan flattened to: $PLAN" >&2
else
    echo "[aletheia] No plan file produced." >&2
    echo "null" > "$PLAN"
fi