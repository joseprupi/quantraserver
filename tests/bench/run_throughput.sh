#!/bin/bash
# Quantra parallel-throughput benchmark — HOST-side sweep driver (informational).
#
# Each worker count is measured in its OWN fresh `docker run --rm` container, so
# the cluster (Envoy + workers + json_server) is reaped automatically when the
# container exits — no teardown code, nothing can leak or hang. The benchmark is
# NOT part of the test gate.
#
# Run from the repo root, on the HOST (it spawns containers):
#   bash tests/bench/run_throughput.sh                  # full sweep + cache scenarios
#   bash tests/bench/run_throughput.sh --sweep 1,2,4,8  # reduced sweep
#   bash tests/bench/run_throughput.sh --requests 120
#   bash tests/bench/run_throughput.sh --scenarios 1    # just the scaling sweep
#
# Requires: docker on PATH and the quantraserver:test image (ships envoy). The
# mounted workspace must already contain a built build/ (the per-N containers do
# not rebuild).

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "${SCRIPT_DIR}/../.." && pwd)"
IMAGE="${QUANTRA_BENCH_IMAGE:-quantraserver:test}"
BENCH="tests/bench/throughput_bench.py"

REQUESTS=240
CACHE_WORKERS=4
SWEEP_RAW=""
# Scenario 3 (swaption smile / SABR cache) is parked: that example payload has a
# curve-overshoot edge and the SABR cache shows no meaningful delta on it. It
# still runs with `--scenarios 3` but is off by default.
SCENARIOS="1,2"
HOST_CPUS="$(nproc 2>/dev/null || echo 8)"

while [ $# -gt 0 ]; do
    case "$1" in
        --sweep)        SWEEP_RAW="$2"; shift 2 ;;
        --requests)     REQUESTS="$2"; shift 2 ;;
        --cache-workers) CACHE_WORKERS="$2"; shift 2 ;;
        --scenarios)    SCENARIOS="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 2 ;;
    esac
done

if ! command -v docker >/dev/null 2>&1; then
    echo "ERROR: docker not found on PATH (this is a host-side driver)."; exit 1
fi

# Default sweep 1,2,4,8,12,24 capped at the host core count.
if [ -z "$SWEEP_RAW" ]; then
    SWEEP=""
    for n in 1 2 4 8 12 24; do
        [ "$n" -le "$HOST_CPUS" ] && SWEEP="$SWEEP $n"
    done
else
    SWEEP="$(echo "$SWEEP_RAW" | tr ',' ' ')"
fi

RESULTS_FILE="$(mktemp)"
trap 'rm -f "$RESULTS_FILE"' EXIT

drun() {
    docker run --rm -v "${WORKSPACE}:/workspace" -w /workspace "$IMAGE" \
        python3 "$BENCH" "$@"
}

echo "##############################################################################"
echo "#  QUANTRA PARALLEL-THROUGHPUT BENCHMARK (informational, hermetic per-N runs)"
echo "#  image=$IMAGE  host_cpus=$HOST_CPUS  sweep=[$SWEEP ]  M=$REQUESTS  cache_workers=$CACHE_WORKERS"
echo "##############################################################################"

want() { echo ",$SCENARIOS," | grep -q ",$1,"; }

# Run one container, echo its full output, and harvest RESULT/SPOTCHECK lines.
measure() {
    local out
    out="$(drun "$@" 2>&1)"
    echo "$out"
    echo "$out" | grep -E '^(RESULT|SPOTCHECK)' >> "$RESULTS_FILE"
}

# --- Linear QuantLib reference (no cluster, quick) ---------------------------
echo ""
echo ">>> Linear QuantLib reference (single-threaded, M=$REQUESTS swaps)"
measure --scenario linear --requests "$REQUESTS"

# --- Scenario 1: worker sweep (heavy swap, cache cold) -----------------------
if want 1; then
    for N in $SWEEP; do
        echo ""
        echo ">>> Scenario 1 | workers=$N | unique curve per request (cache cold)"
        measure --scenario 1 --workers "$N" --requests "$REQUESTS"
    done
fi

# --- Scenario 2: CurveCache OFF vs ON (1 curve, N bonds) ---------------------
if want 2; then
    for C in off on; do
        echo ""
        echo ">>> Scenario 2 | workers=$CACHE_WORKERS | CurveCache $C (identical curve repeated)"
        measure --scenario 2 --workers "$CACHE_WORKERS" --requests "$REQUESTS" --cache "$C"
    done
fi

# --- Scenario 3: SabrCalibrateCache OFF vs ON (swaption smiles) --------------
if want 3; then
    for C in off on; do
        echo ""
        echo ">>> Scenario 3 | workers=$CACHE_WORKERS | SabrCalibrateCache $C (smile reuse=$C)"
        measure --scenario 3 --workers "$CACHE_WORKERS" --requests "$REQUESTS" --cache "$C"
    done
fi

# --- Aggregate ---------------------------------------------------------------
echo ""
echo "=============================================================================="
echo "  AGGREGATED RESULTS"
echo "=============================================================================="
LINEAR_RPS="$(awk -F'rps=' '/scenario=linear/ {split($2,a," "); print a[1]}' "$RESULTS_FILE" | head -1)"
[ -z "$LINEAR_RPS" ] && LINEAR_RPS="0"

if want 1; then
    echo ""
    echo "  Scenario 1 — throughput scaling across workers (heavy swap, cache cold)"
    echo "  Reference: linear single-threaded QuantLib = ${LINEAR_RPS} req/s"
    printf "  %-8s | %-10s | %-12s | %-7s\n" "workers" "req/s" "vs-linear" "errors"
    echo "  -----------------------------------------------------"
    awk -F' ' -v lin="$LINEAR_RPS" '
        /^RESULT scenario=1 / {
            for (i=1;i<=NF;i++){ split($i,kv,"="); v[kv[1]]=kv[2] }
            ratio = (lin>0)? v["rps"]/lin : 0
            printf "  %-8s | %-10.1f | %-11.2fx | %-7s\n", v["workers"], v["rps"], ratio, v["errors"]
        }' "$RESULTS_FILE"
fi

print_cache() {
    local scen="$1" name="$2"
    local off on
    off="$(awk -F' ' -v s="$scen" '$0 ~ "scenario="s" " && /cache=off/ {for(i=1;i<=NF;i++){split($i,kv,"=");v[kv[1]]=kv[2]}; print v["rps"]}' "$RESULTS_FILE" | head -1)"
    on="$(awk -F' ' -v s="$scen" '$0 ~ "scenario="s" " && /cache=on/ {for(i=1;i<=NF;i++){split($i,kv,"=");v[kv[1]]=kv[2]}; print v["rps"]}' "$RESULTS_FILE" | head -1)"
    echo ""
    echo "  Scenario $scen — $name (workers=$CACHE_WORKERS)"
    printf "  OFF: %-8s req/s    ON: %-8s req/s" "${off:-?}" "${on:-?}"
    if [ -n "${off:-}" ] && [ -n "${on:-}" ]; then
        awk -v o="$off" -v n="$on" 'BEGIN{ if(o>0) printf "    ON/OFF = %.2fx\n", n/o; else print "" }'
    else
        echo ""
    fi
}

want 2 && print_cache 2 "CurveCache OFF vs ON"
want 3 && print_cache 3 "SabrCalibrateCache OFF vs ON"

echo ""
echo "  Spot-check(s):"
grep '^SPOTCHECK' "$RESULTS_FILE" | sed 's/^/    /' || true
echo ""
echo "Done (informational; per-N containers reaped on exit)."
