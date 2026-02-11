#!/usr/bin/env bash
#
# Phase 13.5 — Regression Verification Matrix
#
# Runs a standardized test matrix of profile configs and produces a diff
# report against a previously saved baseline.
#
# Usage:
#   bash scripts/regression-test.sh                  # run all configs, compare to baseline
#   bash scripts/regression-test.sh --save-baseline   # run all configs and save as new baseline
#   bash scripts/regression-test.sh --dry-run         # show what would be run without executing
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BINARY="$PROJECT_DIR/build/grebe"
TMP_DIR="$PROJECT_DIR/tmp"
BASELINE_DIR="$TMP_DIR/regression_baseline"

SAVE_BASELINE=false
DRY_RUN=false

for arg in "$@"; do
    case "$arg" in
        --save-baseline) SAVE_BASELINE=true ;;
        --dry-run)       DRY_RUN=true ;;
        *)               echo "Unknown option: $arg"; exit 1 ;;
    esac
done

# --- Prerequisites -----------------------------------------------------------

if ! command -v jq &>/dev/null; then
    echo "ERROR: jq is required but not found. Install with: sudo apt install jq"
    exit 1
fi

if [ ! -x "$BINARY" ]; then
    echo "ERROR: Binary not found at $BINARY"
    echo "Build with: cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
    exit 1
fi

# Warn if not Release build (check CMakeCache if available)
if [ -f "$PROJECT_DIR/build/CMakeCache.txt" ]; then
    BUILD_TYPE=$(grep -m1 'CMAKE_BUILD_TYPE:STRING=' "$PROJECT_DIR/build/CMakeCache.txt" | cut -d= -f2)
    if [ "$BUILD_TYPE" != "Release" ] && [ "$BUILD_TYPE" != "RelWithDebInfo" ]; then
        echo "WARNING: Build type is '$BUILD_TYPE'. Use Release for accurate results."
        echo "  cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build"
        echo ""
    fi
fi

# --- Color helpers ------------------------------------------------------------

if [ -t 1 ]; then
    RED='\033[0;31m'
    YELLOW='\033[0;33m'
    GREEN='\033[0;32m'
    CYAN='\033[0;36m'
    BOLD='\033[1m'
    NC='\033[0m'
else
    RED='' YELLOW='' GREEN='' CYAN='' BOLD='' NC=''
fi

# --- Test matrix --------------------------------------------------------------

# Format: "config_name|cli_flags"
# --minimized: start window iconified to avoid focus-stealing during automated tests
CONFIGS=(
    # V-Sync OFF (throughput)
    "4ch_embedded_novsync|--embedded --channels=4 --ring-size=64M --no-vsync --minimized --profile"
    "4ch_ipc_16k_novsync|--channels=4 --ring-size=64M --block-size=16384 --no-vsync --minimized --profile"
    "4ch_ipc_64k_novsync|--channels=4 --ring-size=64M --block-size=65536 --no-vsync --minimized --profile"
    "8ch_embedded_novsync|--embedded --channels=8 --ring-size=64M --no-vsync --minimized --profile"
    "8ch_ipc_16k_novsync|--channels=8 --ring-size=64M --block-size=16384 --no-vsync --minimized --profile"
    "8ch_ipc_64k_novsync|--channels=8 --ring-size=64M --block-size=65536 --no-vsync --minimized --profile"
    # V-Sync ON (real-use latency/stability)
    "4ch_embedded_vsync|--embedded --channels=4 --ring-size=64M --minimized --profile"
    "4ch_ipc_16k_vsync|--channels=4 --ring-size=64M --block-size=16384 --minimized --profile"
    "8ch_embedded_vsync|--embedded --channels=8 --ring-size=64M --minimized --profile"
    "8ch_ipc_16k_vsync|--channels=8 --ring-size=64M --block-size=16384 --minimized --profile"
)

TIMESTAMP=$(date +%Y%m%d_%H%M%S)
RUN_DIR="$TMP_DIR/regression_$TIMESTAMP"
mkdir -p "$RUN_DIR"

echo -e "${BOLD}========== REGRESSION TEST MATRIX ==========${NC}"
echo "Timestamp : $TIMESTAMP"
echo "Output    : $RUN_DIR"
echo "Baseline  : $([ -d "$BASELINE_DIR" ] && echo "$BASELINE_DIR" || echo "(none)")"
echo "Configs   : ${#CONFIGS[@]}"
echo ""

if $DRY_RUN; then
    echo "--- DRY RUN: showing configs without executing ---"
    for entry in "${CONFIGS[@]}"; do
        IFS='|' read -r name flags <<< "$entry"
        echo "  $name: $BINARY $flags"
    done
    exit 0
fi

# --- Run configs --------------------------------------------------------------

TOTAL_PASS=0
TOTAL_FAIL=0
CONFIG_RESULTS=()

for entry in "${CONFIGS[@]}"; do
    IFS='|' read -r CONFIG_NAME CLI_FLAGS <<< "$entry"

    echo -e "${CYAN}>>> Running: $CONFIG_NAME${NC}"
    echo "    $BINARY $CLI_FLAGS"

    # Record time before run to identify the output file
    BEFORE_TS=$(date +%s)

    EXIT_CODE=0
    # shellcheck disable=SC2086
    "$BINARY" $CLI_FLAGS 2>&1 | tail -20 || EXIT_CODE=$?

    # Find profile JSON created after BEFORE_TS
    PROFILE_JSON=""
    for f in $(ls -t "$TMP_DIR"/profile_*.json 2>/dev/null); do
        if [ "$(stat -c %Y "$f")" -ge "$BEFORE_TS" ]; then
            PROFILE_JSON="$f"
            break
        fi
    done

    if [ -z "$PROFILE_JSON" ]; then
        echo -e "    ${RED}ERROR: No profile JSON found after run${NC}"
        CONFIG_RESULTS+=("$CONFIG_NAME|FAIL|no_json")
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
        continue
    fi

    # Copy to run directory
    cp "$PROFILE_JSON" "$RUN_DIR/$CONFIG_NAME.json"

    # Check overall_pass from JSON
    OVERALL_PASS=$(jq -r '.overall_pass' "$RUN_DIR/$CONFIG_NAME.json")
    if [ "$OVERALL_PASS" = "true" ]; then
        echo -e "    ${GREEN}PASS (exit=$EXIT_CODE)${NC}"
        CONFIG_RESULTS+=("$CONFIG_NAME|PASS|ok")
        TOTAL_PASS=$((TOTAL_PASS + 1))
    else
        echo -e "    ${RED}FAIL (exit=$EXIT_CODE)${NC}"
        CONFIG_RESULTS+=("$CONFIG_NAME|FAIL|profile_fail")
        TOTAL_FAIL=$((TOTAL_FAIL + 1))
    fi
    echo ""
done

# --- Generate report ----------------------------------------------------------

echo ""
echo -e "${BOLD}========== REGRESSION TEST REPORT ==========${NC}"
echo ""

HAS_BASELINE=false
[ -d "$BASELINE_DIR" ] && HAS_BASELINE=true

REGRESSION_DETECTED=false

# JSON summary accumulator
SUMMARY_JSON=$(jq -n '{timestamp: $ts, configs: []}' --arg ts "$TIMESTAMP")

for entry in "${CONFIGS[@]}"; do
    IFS='|' read -r CONFIG_NAME _ <<< "$entry"

    RESULT_FILE="$RUN_DIR/$CONFIG_NAME.json"
    if [ ! -f "$RESULT_FILE" ]; then
        echo -e "${RED}Config: $CONFIG_NAME — NO RESULTS${NC}"
        continue
    fi

    IS_IPC=false
    IS_EMBEDDED=false
    [[ "$CONFIG_NAME" == *"ipc"* ]] && IS_IPC=true
    [[ "$CONFIG_NAME" == *"embedded"* ]] && IS_EMBEDDED=true

    echo -e "${BOLD}Config: $CONFIG_NAME${NC}"

    BASELINE_FILE="$BASELINE_DIR/$CONFIG_NAME.json"
    HAS_CONFIG_BASELINE=false
    $HAS_BASELINE && [ -f "$BASELINE_FILE" ] && HAS_CONFIG_BASELINE=true

    # Process each scenario
    SCENARIO_COUNT=$(jq '.scenarios | length' "$RESULT_FILE")
    CONFIG_JSON=$(jq -n '{name: $name, scenarios: []}' --arg name "$CONFIG_NAME")

    for i in $(seq 0 $((SCENARIO_COUNT - 1))); do
        SCENARIO_NAME=$(jq -r ".scenarios[$i].name" "$RESULT_FILE")
        SCENARIO_PASS=$(jq -r ".scenarios[$i].pass" "$RESULT_FILE")
        FPS_AVG=$(jq -r ".scenarios[$i].results.fps.avg" "$RESULT_FILE")
        DROP_TOTAL=$(jq -r ".scenarios[$i].drop_total" "$RESULT_FILE")
        SG_DROPS=$(jq -r ".scenarios[$i].sg_drop_total // 0" "$RESULT_FILE")
        SEQ_GAPS=$(jq -r ".scenarios[$i].seq_gaps // 0" "$RESULT_FILE")

        # envelope_match_rate: avg or -1 if not present
        ENV_AVG=$(jq -r ".scenarios[$i].results.envelope_match_rate.avg // -1" "$RESULT_FILE")

        # e2e_latency_ms: avg, p99 or 0 if not present
        E2E_AVG=$(jq -r ".scenarios[$i].results.e2e_latency_ms.avg // 0" "$RESULT_FILE")
        E2E_P99=$(jq -r ".scenarios[$i].results.e2e_latency_ms.p99 // 0" "$RESULT_FILE")

        # Format envelope as percentage
        if [ "$ENV_AVG" = "-1" ] || [ "$ENV_AVG" = "null" ]; then
            ENV_STR="N/A"
            ENV_PCT=-1
        else
            ENV_PCT=$(echo "$ENV_AVG * 100" | bc -l 2>/dev/null || echo "0")
            ENV_STR=$(printf "%.1f%%" "$ENV_PCT")
        fi

        # --- Baseline delta ---
        FPS_DELTA_STR=""
        E2E_DELTA_STR=""
        VERDICT="PASS"
        VERDICT_COLOR="$GREEN"

        if $HAS_CONFIG_BASELINE; then
            B_FPS_AVG=$(jq -r ".scenarios[$i].results.fps.avg // 0" "$BASELINE_FILE" 2>/dev/null || echo "0")
            B_ENV_AVG=$(jq -r ".scenarios[$i].results.envelope_match_rate.avg // -1" "$BASELINE_FILE" 2>/dev/null || echo "-1")
            B_E2E_AVG=$(jq -r ".scenarios[$i].results.e2e_latency_ms.avg // 0" "$BASELINE_FILE" 2>/dev/null || echo "0")
            B_DROP=$(jq -r ".scenarios[$i].drop_total // 0" "$BASELINE_FILE" 2>/dev/null || echo "0")

            # Helper: safe bc comparison (returns 0 on error)
            bc_cmp() { echo "$1" | bc -l 2>/dev/null || echo "0"; }

            # FPS delta
            B_FPS_NONZERO=$(bc_cmp "$B_FPS_AVG > 0.001")
            if [ "$B_FPS_NONZERO" = "1" ]; then
                FPS_DELTA=$(echo "scale=2; ($FPS_AVG - $B_FPS_AVG) / $B_FPS_AVG * 100" | bc -l 2>/dev/null || echo "0")
                FPS_SIGN=""
                [ "$(bc_cmp "$FPS_DELTA >= 0")" = "1" ] && FPS_SIGN="+"
                FPS_DELTA_STR=" (baseline: $(printf "%.1f" "$B_FPS_AVG"), ${FPS_SIGN}$(printf "%.1f" "$FPS_DELTA")%)"

                # Regression check: FPS decrease
                FPS_DECREASE=$(echo "scale=2; -1 * $FPS_DELTA" | bc -l 2>/dev/null || echo "0")
                if [ "$(bc_cmp "$FPS_DECREASE > 20")" = "1" ]; then
                    VERDICT="REGR"
                    VERDICT_COLOR="$RED"
                    REGRESSION_DETECTED=true
                elif [ "$(bc_cmp "$FPS_DECREASE > 10")" = "1" ]; then
                    VERDICT="WARN"
                    VERDICT_COLOR="$YELLOW"
                fi
            fi

            # E2E latency delta (skip when either value is near-zero)
            B_E2E_NONZERO=$(bc_cmp "$B_E2E_AVG > 0.001")
            E2E_NONZERO=$(bc_cmp "$E2E_AVG > 0.001")
            if [ "$B_E2E_NONZERO" = "1" ] && [ "$E2E_NONZERO" = "1" ]; then
                E2E_DELTA=$(echo "scale=2; ($E2E_AVG - $B_E2E_AVG) / $B_E2E_AVG * 100" | bc -l 2>/dev/null || echo "0")
                E2E_SIGN=""
                [ "$(bc_cmp "$E2E_DELTA >= 0")" = "1" ] && E2E_SIGN="+"
                E2E_DELTA_STR=" (${E2E_SIGN}$(printf "%.1f" "$E2E_DELTA")%)"

                # Regression check: E2E latency increase
                if [ "$(bc_cmp "$E2E_DELTA > 50")" = "1" ]; then
                    VERDICT="REGR"
                    VERDICT_COLOR="$RED"
                    REGRESSION_DETECTED=true
                elif [ "$(bc_cmp "$E2E_DELTA > 20")" = "1" ]; then
                    [ "$VERDICT" != "REGR" ] && VERDICT="WARN" && VERDICT_COLOR="$YELLOW"
                fi
            fi

            # Regression check: Envelope match decrease (relative to baseline)
            if [ "$ENV_PCT" != "-1" ] && [ "$B_ENV_AVG" != "-1" ] && [ "$B_ENV_AVG" != "null" ]; then
                B_ENV_PCT=$(echo "$B_ENV_AVG * 100" | bc -l 2>/dev/null || echo "0")
                if $IS_EMBEDDED; then
                    # Embedded: REGR if below 99% absolute, WARN if any decrease from baseline
                    if [ "$(bc_cmp "$ENV_PCT < 99")" = "1" ]; then
                        VERDICT="REGR"
                        VERDICT_COLOR="$RED"
                        REGRESSION_DETECTED=true
                    elif [ "$(bc_cmp "$ENV_PCT < $B_ENV_PCT")" = "1" ]; then
                        [ "$VERDICT" != "REGR" ] && VERDICT="WARN" && VERDICT_COLOR="$YELLOW"
                    fi
                elif $IS_IPC; then
                    # IPC: only check for decrease from baseline (absolute floor not applied
                    # since IPC envelope rates are inherently low at some sample rates)
                    if [ "$(bc_cmp "$B_ENV_PCT > 1")" = "1" ]; then
                        # Only check if baseline was meaningful (>1%)
                        if [ "$(bc_cmp "$ENV_PCT < $B_ENV_PCT - 5")" = "1" ]; then
                            VERDICT="REGR"
                            VERDICT_COLOR="$RED"
                            REGRESSION_DETECTED=true
                        elif [ "$(bc_cmp "$ENV_PCT < $B_ENV_PCT")" = "1" ]; then
                            [ "$VERDICT" != "REGR" ] && VERDICT="WARN" && VERDICT_COLOR="$YELLOW"
                        fi
                    fi
                fi
            fi

            # Regression check: New viewer drops in Embedded mode
            if $IS_EMBEDDED && [ "$DROP_TOTAL" -gt 0 ] && [ "$B_DROP" = "0" ]; then
                [ "$VERDICT" != "REGR" ] && VERDICT="WARN" && VERDICT_COLOR="$YELLOW"
            fi
        fi

        # Override verdict if scenario itself failed
        if [ "$SCENARIO_PASS" != "true" ]; then
            VERDICT="FAIL"
            VERDICT_COLOR="$RED"
        fi

        # Build output line
        RATE_LABEL=$(echo "$SCENARIO_NAME" | sed 's/^[0-9]*ch.//')
        DROP_STR="drops=$DROP_TOTAL"
        if $IS_IPC; then
            DROP_STR="drops=$DROP_TOTAL sg=$SG_DROPS gaps=$SEQ_GAPS"
        fi

        printf "  %-10s FPS=%-7.1f%s  %s  env=%s  e2e=%.1fms%s  " \
            "$RATE_LABEL" "$FPS_AVG" "$FPS_DELTA_STR" "$DROP_STR" "$ENV_STR" "$E2E_AVG" "$E2E_DELTA_STR"
        echo -e "${VERDICT_COLOR}${VERDICT}${NC}"

        # Accumulate JSON
        CONFIG_JSON=$(echo "$CONFIG_JSON" | jq --arg sn "$SCENARIO_NAME" --arg v "$VERDICT" \
            --argjson fps "$FPS_AVG" --argjson drops "$DROP_TOTAL" \
            --argjson env "$ENV_AVG" --argjson e2e "$E2E_AVG" --argjson e2ep99 "$E2E_P99" \
            --argjson sg "$SG_DROPS" --argjson gaps "$SEQ_GAPS" \
            '.scenarios += [{name: $sn, fps_avg: $fps, drop_total: $drops, sg_drops: $sg, seq_gaps: $gaps, envelope_avg: $env, e2e_avg: $e2e, e2e_p99: $e2ep99, verdict: $v}]')
    done

    SUMMARY_JSON=$(echo "$SUMMARY_JSON" | jq --argjson c "$CONFIG_JSON" '.configs += [$c]')
    echo ""
done

# --- Overall summary ----------------------------------------------------------

TOTAL=$((TOTAL_PASS + TOTAL_FAIL))
if [ "$TOTAL_FAIL" -eq 0 ] && ! $REGRESSION_DETECTED; then
    echo -e "${GREEN}${BOLD}Overall: PASS ($TOTAL_PASS/$TOTAL configs passed, 0 regressions)${NC}"
    OVERALL_EXIT=0
else
    FAIL_MSG=""
    [ "$TOTAL_FAIL" -gt 0 ] && FAIL_MSG="$TOTAL_FAIL/$TOTAL failed"
    $REGRESSION_DETECTED && FAIL_MSG="${FAIL_MSG:+$FAIL_MSG, }regressions detected"
    echo -e "${RED}${BOLD}Overall: FAIL ($FAIL_MSG)${NC}"
    OVERALL_EXIT=1
fi
echo -e "${BOLD}============================================${NC}"

# Write JSON summary
SUMMARY_JSON=$(echo "$SUMMARY_JSON" | jq \
    --argjson pass "$TOTAL_PASS" --argjson fail "$TOTAL_FAIL" \
    --argjson regr "$([ "$REGRESSION_DETECTED" = true ] && echo true || echo false)" \
    '. + {total_pass: $pass, total_fail: $fail, regression_detected: $regr}')
echo "$SUMMARY_JSON" | jq '.' > "$RUN_DIR/summary.json"
echo ""
echo "Summary JSON: $RUN_DIR/summary.json"

# --- Save baseline ------------------------------------------------------------

if $SAVE_BASELINE; then
    echo ""
    echo -e "${CYAN}Saving baseline to $BASELINE_DIR${NC}"
    rm -rf "$BASELINE_DIR"
    cp -r "$RUN_DIR" "$BASELINE_DIR"
    echo "Baseline saved."
fi

exit $OVERALL_EXIT
