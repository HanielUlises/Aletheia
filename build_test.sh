#!/usr/bin/env bash
# build_and_test.sh — build aletheia and run the team-3 smoke-test suite
#
# Usage:
#   ./build_and_test.sh [--repo <path>] [--logs <path>] [--jobs N] [--verbose]
#
# Defaults:
#   --repo    directory containing CMakeLists.txt  (default: script's own dir)
#   --logs    where to write per-test .log files   (default: <repo>/smoke-logs)
#   --jobs    parallel make jobs                   (default: nproc)
#   --verbose stream planner stderr live instead of capturing it

set -euo pipefail

# ── helpers ──────────────────────────────────────────────────────────────────

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
BOLD='\033[1m'; RESET='\033[0m'

die()  { echo -e "${RED}[fatal]${RESET} $*" >&2; exit 1; }
info() { echo -e "${BOLD}[build_and_test]${RESET} $*"; }
ok()   { echo -e "${GREEN}[PASS]${RESET} $1"; }
fail() { echo -e "${RED}[FAIL]${RESET} $1"; }
warn() { echo -e "${YELLOW}[WARN]${RESET} $1"; }

# ── argument parsing ──────────────────────────────────────────────────────────

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$SCRIPT_DIR"
LOGS=""
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
VERBOSE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --repo)    REPO="$2";  shift 2 ;;
        --logs)    LOGS="$2";  shift 2 ;;
        --jobs)    JOBS="$2";  shift 2 ;;
        --verbose) VERBOSE=1;  shift   ;;
        *) die "unknown argument: $1" ;;
    esac
done

[[ -f "$REPO/CMakeLists.txt" ]] || die "CMakeLists.txt not found under $REPO"

LOGS="${LOGS:-$REPO/smoke-logs}"
BUILD="$REPO/build"
PLANNER="$BUILD/epistemic_planner"
ALETHEIA="$REPO/aletheia.sh"
BENCHMARK="$REPO/benchmark"

info "Configuring (cmake) …"
cmake -B "$BUILD" -S "$REPO" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
      2>&1 | sed 's/^/  [cmake] /'

info "Building with $JOBS job(s) …"
cmake --build "$BUILD" -j"$JOBS" 2>&1 | sed 's/^/  [make]  /'

[[ -x "$PLANNER" ]] || die "build finished but binary not found: $PLANNER"
info "Binary ready: $PLANNER"
echo ""


[[ -d "$BENCHMARK" ]] || die "benchmark/ directory not found under $REPO"

mapfile -t SMOKE_FILES < <(
    find "$BENCHMARK" -name "smoke-test.epddl" | sort
)

[[ ${#SMOKE_FILES[@]} -gt 0 ]] || die "no smoke-test.epddl files found under $BENCHMARK"

info "Found ${#SMOKE_FILES[@]} smoke test(s)"
echo ""

if ! command -v plank &>/dev/null; then
    warn "'plank' not on PATH — plan-verification step will be skipped"
    HAS_PLANK=0
else
    HAS_PLANK=1
fi

mkdir -p "$LOGS"

PASS=0; FAIL=0; SKIP=0
declare -a FAILED_NAMES=()

for SMOKE in "${SMOKE_FILES[@]}"; do
    PROB_DIR="$(dirname "$SMOKE")"
    DOMAIN="$(dirname "$PROB_DIR")/domain.epddl"
    REL="$(realpath --relative-to="$BENCHMARK" "$SMOKE")"

    if [[ ! -f "$DOMAIN" ]]; then
        DOMAIN="$(dirname "$(dirname "$PROB_DIR")")/domain.epddl"
    fi

    if [[ ! -f "$DOMAIN" ]]; then
        warn "domain not found for $REL — skipping"
        (( SKIP++ )) || true
        continue
    fi

    SAFE_NAME="${REL//\//_}"
    SAFE_NAME="${SAFE_NAME%.epddl}"
    LOG="$LOGS/${SAFE_NAME}.log"
    TASK_JSON="$(mktemp /tmp/aletheia-task-XXXXXX.json)"
    PLAN_JSON="$(mktemp /tmp/aletheia-plan-XXXXXX.json)"


    GROUND_OK=0
    if [[ $HAS_PLANK -eq 1 ]]; then
        if plank "$SMOKE" --output-json "$TASK_JSON" &>"$LOG" 2>&1; then
            GROUND_OK=1
        fi
    else
        GROUND_OK=2
    fi


    printf "  %-60s " "$REL"

    if [[ $GROUND_OK -eq 2 ]]; then
        if [[ $VERBOSE -eq 1 ]]; then
            "$ALETHEIA" "$SMOKE" "$PLAN_JSON" --heuristic ed --timeout 120 \
                2>&1 | tee -a "$LOG"
        else
            "$ALETHEIA" "$SMOKE" "$PLAN_JSON" --heuristic ed --timeout 120 \
                >> "$LOG" 2>&1
        fi
        EXIT_CODE=$?
    elif [[ $GROUND_OK -eq 1 ]]; then
        if [[ $VERBOSE -eq 1 ]]; then
            "$PLANNER" --task "$TASK_JSON" --plan "$PLAN_JSON" \
                --heuristic ed --timeout 120 \
                2>&1 | tee -a "$LOG"
        else
            "$PLANNER" --task "$TASK_JSON" --plan "$PLAN_JSON" \
                --heuristic ed --timeout 120 \
                >> "$LOG" 2>&1
        fi
        EXIT_CODE=$?
    else
        warn "grounding failed for $REL"
        (( SKIP++ )) || true
        rm -f "$TASK_JSON" "$PLAN_JSON"
        continue
    fi


    PLAN_CONTENT=""
    [[ -f "$PLAN_JSON" ]] && PLAN_CONTENT="$(cat "$PLAN_JSON")"

    if [[ "$PLAN_CONTENT" == "null" || -z "$PLAN_CONTENT" ]]; then
        fail "$REL"
        (( FAIL++ )) || true
        FAILED_NAMES+=("$REL  →  no plan (null)")
    else
        VERIFIED=1
        if [[ $HAS_PLANK -eq 1 && $GROUND_OK -eq 1 ]]; then
            if ! plank "$SMOKE" --verify-plan "$PLAN_JSON" >> "$LOG" 2>&1; then
                VERIFIED=0
            fi
        fi

        if [[ $VERIFIED -eq 1 ]]; then
            DEPTH=$(grep -oP 'depth \K[0-9]+' "$LOG" | tail -1)
            EXPANDED=$(grep -oP 'Expanded=\K[0-9]+' "$LOG" | tail -1)
            EXTRA=""
            [[ -n "$DEPTH" ]]    && EXTRA=" depth=$DEPTH"
            [[ -n "$EXPANDED" ]] && EXTRA="$EXTRA expanded=$EXPANDED"
            ok "$REL$EXTRA"
            (( PASS++ )) || true
        else
            fail "$REL  (plan found but validator rejected it)"
            (( FAIL++ )) || true
            FAILED_NAMES+=("$REL  →  invalid plan")
        fi
    fi

    rm -f "$TASK_JSON" "$PLAN_JSON"
done


echo ""
echo "----------------------------------------------------------------"
TOTAL=$(( PASS + FAIL + SKIP ))
echo -e "  Results: ${GREEN}${PASS} passed${RESET}  ${RED}${FAIL} failed${RESET}  ${YELLOW}${SKIP} skipped${RESET}  /  ${TOTAL} total"
echo ""

if [[ ${#FAILED_NAMES[@]} -gt 0 ]]; then
    echo -e "  ${RED}Failed tests:${RESET}"
    for name in "${FAILED_NAMES[@]}"; do
        echo "    • $name"
    done
    echo ""
fi

echo "  Logs written to: $LOGS"
echo "----------------------------------------------------------------"
echo ""

[[ $FAIL -eq 0 ]]