#!/bin/bash
# Quantra Curve Cache Benchmark
#
# Usage:
#   ./tests/run_bench.sh           # run all 3 modes
#   ./tests/run_bench.sh bond      # single mode
#   ./tests/run_bench.sh swap
#   ./tests/run_bench.sh swap2

GRPC_PORT=50055
HTTP_PORT=8080
N_REQUESTS=200
WARMUP=5

PID_GRPC=""
PID_HTTP=""

kill_servers() {
    [ -n "$PID_HTTP" ] && [ -d "/proc/$PID_HTTP" ] && kill -9 $PID_HTTP 2>/dev/null && wait $PID_HTTP 2>/dev/null
    [ -n "$PID_GRPC" ] && [ -d "/proc/$PID_GRPC" ] && kill -9 $PID_GRPC 2>/dev/null && wait $PID_GRPC 2>/dev/null
    PID_HTTP=""
    PID_GRPC=""
}

wait_for_http() {
    for i in $(seq 1 30); do
        curl -s http://localhost:$HTTP_PORT/health > /dev/null 2>&1 && return 0
        sleep 0.3
    done
    echo "ERROR: HTTP server not ready"; return 1
}

start_servers() {
    local cache_enabled=$1
    env -i PATH="$PATH" LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-}" \
        QUANTRA_CURVE_CACHE_ENABLED=$cache_enabled \
        QUANTRA_CURVE_CACHE_LOG=$( [ "$cache_enabled" = "1" ] && echo "1" || echo "0" ) \
        ./build/server/sync_server $GRPC_PORT > /tmp/quantra_bench.log 2>&1 &
    PID_GRPC=$!
    sleep 1
    [ ! -d "/proc/$PID_GRPC" ] && echo "FATAL: gRPC server died" && return 1
    ./build/jsonserver/json_server localhost:$GRPC_PORT $HTTP_PORT > /dev/null 2>&1 &
    PID_HTTP=$!
    wait_for_http || return 1
    return 0
}

run_one_mode() {
    local mode=$1
    echo ""
    echo "============================================"
    echo "  Benchmarking: $mode"
    echo "============================================"

    # NO CACHE
    echo "  [1/2] no-cache..."
    start_servers 0 || return 1
    RESULT_NC=$(python3 tests/bench_cache.py --url http://localhost:$HTTP_PORT --tag "no-cache" --mode $mode -n $N_REQUESTS --warmup $WARMUP 2>&1)
    kill_servers; sleep 3

    # WITH CACHE
    echo "  [2/2] with-cache..."
    start_servers 1 || return 1
    RESULT_C=$(python3 tests/bench_cache.py --url http://localhost:$HTTP_PORT --tag "with-cache" --mode $mode -n $N_REQUESTS --warmup $WARMUP 2>&1)
    kill_servers; sleep 2

    # Extract stats
    MEAN_NC=$(echo "$RESULT_NC" | grep "Mean:" | awk '{print $2}')
    MEAN_C=$(echo "$RESULT_C" | grep "Mean:" | awk '{print $2}')

    # Debug: if no stats, show raw output
    if [ -z "$MEAN_NC" ]; then
        echo "  [DEBUG] no-cache output:"
        echo "$RESULT_NC" | tail -20
        echo ""
    fi
    if [ -z "$MEAN_C" ]; then
        echo "  [DEBUG] with-cache output:"
        echo "$RESULT_C" | tail -20
        echo ""
    fi

    MED_NC=$(echo "$RESULT_NC" | grep "Median:" | awk '{print $2}')
    MED_C=$(echo "$RESULT_C" | grep "Median:" | awk '{print $2}')
    P95_NC=$(echo "$RESULT_NC" | grep "P95:" | awk '{print $2}')
    P95_C=$(echo "$RESULT_C" | grep "P95:" | awk '{print $2}')
    RPS_NC=$(echo "$RESULT_NC" | grep "Throughput:" | awk '{print $2}')
    RPS_C=$(echo "$RESULT_C" | grep "Throughput:" | awk '{print $2}')
    SPEEDUP=$(echo "scale=1; $MEAN_NC / $MEAN_C" | bc 2>/dev/null || echo "?")

    # Cache log excerpt
    CACHE_LOG=$(grep CurveCache /tmp/quantra_bench.log 2>/dev/null | head -6)

    # Store for final summary
    RESULTS+=("$mode|$MEAN_NC|$MEAN_C|$MED_NC|$MED_C|$P95_NC|$P95_C|$RPS_NC|$RPS_C|$SPEEDUP")
    CACHE_LOGS+=("$mode|$CACHE_LOG")
}

trap kill_servers EXIT
pkill -9 -f sync_server 2>/dev/null; pkill -9 -f json_server 2>/dev/null; sleep 2

# Determine which modes to run
if [ -n "$1" ]; then
    ALL_MODES="$1"
else
    ALL_MODES="bond swap"
fi

declare -a RESULTS
declare -a CACHE_LOGS

echo "============================================"
echo "  Quantra Curve Cache Benchmark Suite"
echo "  Modes: $ALL_MODES | $N_REQUESTS req/run"
echo "============================================"

for mode in $ALL_MODES; do
    run_one_mode $mode
done

# ---- FINAL REPORT ----
echo ""
echo ""
echo "╔══════════════════════════════════════════════════════════════════════════════╗"
echo "║                        CURVE CACHE BENCHMARK RESULTS                       ║"
echo "╠═══════════╦════════════════════╦════════════════════╦═══════════════════════╣"
echo "║           ║     No Cache       ║     With Cache     ║                       ║"
echo "║   Mode    ╠══════╦══════╦══════╬══════╦══════╦══════╣  Speedup              ║"
echo "║           ║ Mean ║  Med ║  P95 ║ Mean ║  Med ║  P95 ║                       ║"
echo "╠═══════════╬══════╬══════╬══════╬══════╬══════╬══════╬═══════════════════════╣"

for entry in "${RESULTS[@]}"; do
    IFS='|' read -r mode mnc mc mednc medc p95nc p95c rpsnc rpsc spd <<< "$entry"
    printf "║ %-9s ║%5s ║%5s ║%5s ║%5s ║%5s ║%5s ║ %5sx (%s→%s req/s) ║\n" \
        "$mode" "$mnc" "$mednc" "$p95nc" "$mc" "$medc" "$p95c" "$spd" "$rpsnc" "$rpsc"
done

echo "╚═══════════╩══════╩══════╩══════╩══════╩══════╩══════╩═══════════════════════╝"
echo ""

for entry in "${CACHE_LOGS[@]}"; do
    IFS='|' read -r mode rest <<< "$entry"
    echo "  [$mode] cache log:"
    echo "$rest" | head -4 | sed 's/^/    /'
    echo ""
done