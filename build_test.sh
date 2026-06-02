#!/usr/bin/env bash
# build_test.sh build aletheia and run the local benchmark suite
#
# Usage:
#   ./build_test.sh [--repo <path>] [--benchmarks <path>] [--logs <path>]
#                       [--jobs N] [--timeout N] [--heuristic ed|ug|ks|wc]
#                       [--verbose] [--no-build]
#
# Each subdirectory of benchmarks/ that contains a grounded task JSON
# (identified by the "planning-task-info" key) is run as one test.
# Folders with multiple task JSONs (e.g. amc1 has both amc-problem.json
# and problem_1.json which are identical) are deduped — only the first
# alphabetically is used.

set -euo pipefail

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; RESET='\033[0m'

die()  { echo -e "${RED}[fatal]${RESET} $*" >&2; exit 1; }
info() { echo -e "${BOLD}[aletheia]${RESET} $*"; }
ok()   { echo -e "  ${GREEN}PASS${RESET}  $*"; }
fail() { echo -e "  ${RED}FAIL${RESET}  $*"; }
warn() { echo -e "  ${YELLOW}WARN${RESET}  $*"; }
skip() { echo -e "  ${CYAN}SKIP${RESET}  $*"; }

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR"
BENCHMARKS=""
LOGS=""
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
TIMEOUT=120
HEURISTIC="ed"
VERBOSE=0
NO_BUILD=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo)        REPO="$2";       shift 2 ;;
        --benchmarks)  BENCHMARKS="$2"; shift 2 ;;
        --logs)        LOGS="$2";       shift 2 ;;
        --jobs)        JOBS="$2";       shift 2 ;;
        --timeout)     TIMEOUT="$2";    shift 2 ;;
        --heuristic)   HEURISTIC="$2";  shift 2 ;;
        --verbose)     VERBOSE=1;       shift   ;;
        --no-build)    NO_BUILD=1;      shift   ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ -f "$REPO/CMakeLists.txt" ]] || die "CMakeLists.txt not found under $REPO"

BENCHMARKS="${BENCHMARKS:-$REPO/benchmarks}"
LOGS="${LOGS:-$REPO/smoke-logs}"
BUILD="$REPO/build"
PLANNER="$BUILD/epistemic_planner"

[[ -d "$BENCHMARKS" ]] || die "benchmarks directory not found: $BENCHMARKS"

if [[ $NO_BUILD -eq 0 ]]; then
    info "Configuring …"
    cmake -B "$BUILD" -S "$REPO" \
          -DCMAKE_BUILD_TYPE=Release \
          -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
          2>&1 | sed 's/^/  [cmake] /'

    info "Building ($JOBS jobs) …"
    cmake --build "$BUILD" -j"$JOBS" 2>&1 | sed 's/^/  [make]  /'
fi

[[ -x "$PLANNER" ]] || die "binary not found: $PLANNER  (run without --no-build?)"
info "Binary: $PLANNER"
echo ""

declare -a TASKS=()
declare -a TASK_NAMES=() 

for folder in $(find "$BENCHMARKS" -mindepth 1 -maxdepth 1 -type d | sort); do
    name="$(basename "$folder")"
    first=""
    for f in $(ls "$folder"/*.json 2>/dev/null | sort); do
        if grep -q '"planning-task-info"' "$f" 2>/dev/null; then
            first="$f"
            break
        fi
    done
    if [[ -z "$first" ]]; then
        skip "$name  (no grounded task JSON)"
        continue
    fi
    TASKS+=("$first")
    TASK_NAMES+=("$name")
done

echo ""
info "Found ${#TASKS[@]} benchmark(s)"
echo ""

mkdir -p "$LOGS"

PASS=0; FAIL=0; TOTAL=0
declare -a FAILED_NAMES=()

for i in "${!TASKS[@]}"; do
    TASK="${TASKS[$i]}"
    NAME="${TASK_NAMES[$i]}"
    LOG="$LOGS/${NAME}.log"
    PLAN="$(mktemp /tmp/aletheia-plan-XXXXXX.json)"

    (( TOTAL++ )) || true
    printf "  %-28s  " "$NAME"

    START=$(date +%s%N)

    if [[ $VERBOSE -eq 1 ]]; then
        "$PLANNER" \
            --task      "$TASK" \
            --plan      "$PLAN" \
            --heuristic "$HEURISTIC" \
            --timeout   "$TIMEOUT" \
            2>&1 | tee "$LOG"
    else
        "$PLANNER" \
            --task      "$TASK" \
            --plan      "$PLAN" \
            --heuristic "$HEURISTIC" \
            --timeout   "$TIMEOUT" \
            > "$LOG" 2>&1 || true
    fi

    END=$(date +%s%N)
    ELAPSED=$(( (END - START) / 1000000 ))


    PLAN_CONTENT=""
    [[ -f "$PLAN" ]] && PLAN_CONTENT="$(cat "$PLAN")"

    STRATEGY=$(grep -oP '\[main\] Strategy: \K.*' "$LOG" | head -1)
    FALLBACK=$(grep -oP '\[main\] .*falling back.*' "$LOG" | head -1 || true)
    DEPTH=$(grep -oP '(?:depth |Trying depth )\K[0-9]+' "$LOG" | tail -1 || true)
    EXPANDED=$(grep -oP 'Expanded=\K[0-9]+' "$LOG" | tail -1 || true)
    TIMEOUT_HIT=$(grep -c 'TIMEOUT' "$LOG" 2>/dev/null || echo 0)

    STATS=""
    [[ -n "$DEPTH" ]]    && STATS="${STATS} depth=${DEPTH}"
    [[ -n "$EXPANDED" ]] && STATS="${STATS} exp=${EXPANDED}"
    STATS="${STATS} (${ELAPSED}ms)"

    if [[ "$PLAN_CONTENT" == "null" || -z "$PLAN_CONTENT" ]]; then
        echo -e "${RED}FAIL${RESET}  ${NAME}${STATS}"
        if [[ $TIMEOUT_HIT -gt 0 ]]; then
            echo "        → timeout"
        else
            LAST=$(tail -3 "$LOG" | tr '\n' ' ')
            echo "        → $LAST"
        fi
        (( FAIL++ )) || true
        FAILED_NAMES+=("$NAME")
    else
        if echo "$PLAN_CONTENT" | python3 -c "import json,sys; d=json.load(sys.stdin); exit(0 if isinstance(d,dict) and 'action' in d else 1)" 2>/dev/null; then
            PLAN_TYPE="conditional"
        else
            N=$(echo "$PLAN_CONTENT" | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d))" 2>/dev/null || echo "?")
            PLAN_TYPE="linear(${N})"
        fi
        echo -e "${GREEN}PASS${RESET}  ${NAME}  [${PLAN_TYPE}]${STATS}"
        (( PASS++ )) || true
    fi

    rm -f "$PLAN"
done

echo ""
echo "---------------------------------------------------------------------"
echo -e "  ${GREEN}${PASS} passed${RESET}   ${RED}${FAIL} failed${RESET}   ${TOTAL} total"

if [[ ${#FAILED_NAMES[@]} -gt 0 ]]; then
    echo ""
    echo -e "  ${RED}Failed:${RESET}"
    for n in "${FAILED_NAMES[@]}"; do
        echo "    • $n  →  $LOGS/${n}.log"
    done
fi

echo ""
echo "  Logs: $LOGS"
echo "---------------------------------------------------------------------"
echo ""

[[ $FAIL -eq 0 ]]