#!/usr/bin/env bash
# run_benchmarks.sh — aletheia-planner benchmark runner

set -euo pipefail

PLANNER="$(dirname "$0")/build/epistemic_planner"
BENCHMARKS_DIR="$(dirname "$0")/benchmarks"
RESULTS_DIR="$(dirname "$0")/results"

TIMEOUT=60

HEURISTICS=(
    "ug"
    "ed"
    "ks"
    "wc"
)

while [[ $# -gt 0 ]]; do
    case "$1" in
        --timeout)
            TIMEOUT="$2"
            shift 2
            ;;
        --planner)
            PLANNER="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1"
            exit 1
            ;;
    esac
done

if [[ ! -x "$PLANNER" ]]; then
    echo "Error: planner not found or not executable: $PLANNER"
    echo "Set --planner path/to/epistemic_planner or build first."
    exit 1
fi

mkdir -p "$RESULTS_DIR"

CSV="$RESULTS_DIR/benchmark_results.csv"

echo "benchmark,problem,frame,strategy,heuristic,worlds,actions,expanded,generated,plan_length,time_ms,solved" > "$CSV"

BENCHMARKS=(
    "amc1        problem_1.json"
    "cn5         cn5.json"
    "coin1       problem_1.json"
    "coin2       problem_2.json"
    "coin3       problem_3.json"
    "coin4       problem_4.json"
    "coin5       problem_5.json"
    "gossip1     problem_1.json"
    "grapevine1  problem_1.json"
    "sally-anne  problem_1.json"
    "backdoor    backdoor-problem.json"
    "whisper     whisper-problem.json"
    "muddy1      muddy-children-problem.json"
    "muddy3      muddy-children-problem-3.json"
)

for heuristic in "${HEURISTICS[@]}"; do

    echo
    echo "══════════════════════════════════════════"
    echo "Running heuristic: $heuristic"
    echo "══════════════════════════════════════════"
    echo

    for entry in "${BENCHMARKS[@]}"; do

        read -r bench problem <<< "$entry"

        task="$BENCHMARKS_DIR/$bench/$problem"

        if [[ ! -f "$task" ]]; then
            echo "  [skip] $bench/$problem — file not found"
            continue
        fi

        mkdir -p "$RESULTS_DIR/$heuristic/$bench"

        plan_out="$RESULTS_DIR/$heuristic/$bench/plan.json"

        echo "── $bench/$problem [$heuristic]"

        rm -f "$plan_out"

        log_file=$(mktemp)

        start_ms=$(date +%s%3N)

        timeout "${TIMEOUT}s" \
            "$PLANNER" \
            --task "$task" \
            --plan "$plan_out" \
            --heuristic "$heuristic" \
            > /dev/null 2> "$log_file" || true

        end_ms=$(date +%s%3N)

        stderr_log=$(cat "$log_file")

        rm -f "$log_file"

        time_ms=$(( end_ms - start_ms ))

        echo "$stderr_log"

        frame=$(echo "$stderr_log" \
            | grep -oP '(?<=Frame: )\S+' \
            | head -1 || echo "unknown")

        strategy=$(echo "$stderr_log" \
            | grep -oP '(?<=Strategy: )[^\n]+' \
            | head -1 || echo "")

        if [[ -z "$strategy" ]]; then
            strategy=$(echo "$stderr_log" \
                | grep -oP '(?<=Mode: )[^\n]+' \
                | head -1 || echo "unknown")
        fi

        strategy=$(echo "$strategy" | sed 's/ (.*//')

        worlds=$(echo "$stderr_log" \
            | grep -oP '\d+(?= worlds)' \
            | head -1 || echo "0")

        actions=$(echo "$stderr_log" \
            | grep -oP '\d+(?= actions)' \
            | head -1 || echo "0")

        expanded=$(echo "$stderr_log" \
            | grep -oP '(?<=Expanded=)\d+' \
            | tail -1 || echo "0")

        generated=$(echo "$stderr_log" \
            | grep -oP '(?<=Generated=)\d+' \
            | tail -1 || echo "0")

        plan_len=$(echo "$stderr_log" \
            | grep -oP '(?<=Length=)\d+' \
            | head -1 || echo "")

        if [[ -z "$plan_len" ]] && [[ -f "$plan_out" ]]; then

            plan_len=$(python3 -c "
import json

try:
    with open('$plan_out') as f:
        data = json.load(f)

    if data is None:
        print(0)

    elif isinstance(data, list):
        print(len(data))

    else:
        def count(node):
            if node is None:
                return 0

            n = 1

            for b in node.get('branches', []):
                n += count(b.get('subtree'))

            return n

        print(count(data))

except Exception:
    print(0)
" 2>/dev/null || echo "0")
        fi

        plan_len=${plan_len:-0}

        if \
            echo "$stderr_log" | grep -qE "No solution found|wrote null|Search exhausted|Timeout" \
            || [[ ! -f "$plan_out" ]] \
            || [[ ! -s "$plan_out" ]] \
            || grep -q "^null$" "$plan_out"
        then
            solved="false"
            plan_len="0"
        else
            solved="true"
        fi

        echo "$bench,$problem,$frame,$strategy,$heuristic,$worlds,$actions,$expanded,$generated,$plan_len,$time_ms,$solved" >> "$CSV"

        echo "  → solved=$solved  length=$plan_len  expanded=$expanded  time=${time_ms}ms"

        echo
    done
done

echo "══════════════════════════════════════════"
echo "Results written to: $CSV"
echo

column -t -s, "$CSV"