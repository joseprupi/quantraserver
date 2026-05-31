#!/bin/bash
# Quantra Test Suite

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD="${WORKSPACE}/build"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

PASSED=0
FAILED=0
GRPC_PID=""
JSON_PID=""
GRPC_CACHE_PID=""
JSON_CACHE_PID=""
declare -a SUITE_NAMES
declare -a SUITE_ROLES
declare -a SUITE_STATUS
declare -a SUITE_CASES
declare -a SUITE_CASE_LABELS

cleanup() {
    echo ""
    echo "Cleaning up..."
    [ -n "$JSON_PID" ] && kill $JSON_PID 2>/dev/null
    [ -n "$GRPC_PID" ] && kill $GRPC_PID 2>/dev/null
    [ -n "$JSON_CACHE_PID" ] && kill $JSON_CACHE_PID 2>/dev/null
    [ -n "$GRPC_CACHE_PID" ] && kill $GRPC_CACHE_PID 2>/dev/null
    # Also kill any orphaned servers
    pkill -f "sync_server 50051" 2>/dev/null
    pkill -f "json_server localhost:50051" 2>/dev/null
    pkill -f "sync_server 50052" 2>/dev/null
    pkill -f "json_server localhost:50052" 2>/dev/null
}

trap cleanup EXIT

extract_case_count() {
    local kind="$1"
    local logfile="$2"
    case "$kind" in
        gtest)
            awk '/\[  PASSED  \] [0-9]+ tests\./ {count=$4} END {if (count == "") count="?"; print count}' "$logfile"
            ;;
        json)
            awk '/^TOTAL SCENARIOS:/ {split($3, a, "/"); total=a[2]} END {if (total == "") total="?"; print total}' "$logfile"
            ;;
        pytest)
            # Parse pytest's summary line, e.g. "=== 37 passed in 1.23s ===" ->
            # 37. tail -1 picks the final summary if collection/warnings emit
            # earlier "N passed" fragments. Defaults to ? when absent (failure).
            local n
            n="$(grep -oE '[0-9]+ passed' "$logfile" | tail -1 | grep -oE '[0-9]+')"
            [ -n "$n" ] && echo "$n" || echo "?"
            ;;
        python)
            awk '/^Total scenarios:/ {split($3, a, "/"); total=a[2]} END {if (total == "") total="?"; print total}' "$logfile"
            ;;
        boundary)
            # Parse "Pricer boundary OK (N files checked)" → N. Default to ?
            # when the success line isn't present (e.g. on failure).
            local n
            n="$(sed -n 's/^Pricer boundary OK (\([0-9]\+\) files checked)$/\1/p' "$logfile" | head -1)"
            [ -n "$n" ] && echo "$n" || echo "?"
            ;;
        concurrency)
            # Concurrency test prints "TOTAL SCENARIOS: passed/total" (total =
            # number of concurrent requests fired at one endpoint).
            awk '/^TOTAL SCENARIOS:/ {split($3, a, "/"); total=a[2]} END {if (total == "") total="?"; print total}' "$logfile"
            ;;
        cache)
            # Parse "Cache transparency OK (N comparisons checked)" → N (number
            # of product cache-OFF vs cache-ON comparisons). Default ? when the
            # success line isn't present (e.g. on failure).
            local n
            n="$(sed -n 's/.*Cache transparency OK (\([0-9]\+\) comparisons checked).*/\1/p' "$logfile" | head -1)"
            [ -n "$n" ] && echo "$n" || echo "?"
            ;;
        *)
            echo "?"
            ;;
    esac
}

# Emit one stable, greppable line describing a finished suite. No color codes
# or box characters: downstream tooling does `grep '^RESULT '` and parses with
# simple key=value splitting. A genuinely unparsed count (extract_case_count
# returned "?") is reported as "unknown" so the value still parses cleanly.
emit_result() {
    local idx="$1"
    local count="${SUITE_CASES[$idx]}"
    [ "$count" = "?" ] && count="unknown"
    printf 'RESULT suite=%s name="%s" status=%s count=%s unit=%s\n' \
        "$idx" "${SUITE_NAMES[$idx]}" "${SUITE_STATUS[$idx]}" "$count" "${SUITE_CASE_LABELS[$idx]}"
}

run_test() {
    local idx="$1"
    local name="$2"
    local role="$3"
    local kind="$4"
    local cmd="$5"
    local logfile
    logfile="$(mktemp)"
    echo ""
    echo "════════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════════"
    echo "  Role: $role"
    # Capture suite output and exit code separately. Previously this was
    # `if bash -lc ... | tee ...; then`, which evaluated tee's exit (always 0)
    # under the default no-pipefail shell — every crashed suite reported PASS.
    local rc
    bash -lc "$cmd" > "$logfile" 2>&1
    rc=$?
    cat "$logfile"
    if [ "$rc" -eq 0 ]; then
        echo -e "${GREEN}✓ PASSED${NC}"
        SUITE_STATUS[$idx]="PASS"
        ((PASSED++))
    else
        echo -e "${RED}✗ FAILED${NC} (exit $rc)"
        SUITE_STATUS[$idx]="FAIL"
        ((FAILED++))
    fi
    SUITE_NAMES[$idx]="$name"
    SUITE_ROLES[$idx]="$role"
    SUITE_CASES[$idx]="$(extract_case_count "$kind" "$logfile")"
    case "$kind" in
        gtest) SUITE_CASE_LABELS[$idx]="cases" ;;
        json) SUITE_CASE_LABELS[$idx]="scenarios" ;;
        pytest) SUITE_CASE_LABELS[$idx]="tests" ;;
        python) SUITE_CASE_LABELS[$idx]="scenarios" ;;
        boundary) SUITE_CASE_LABELS[$idx]="files" ;;
        concurrency) SUITE_CASE_LABELS[$idx]="requests" ;;
        cache) SUITE_CASE_LABELS[$idx]="comparisons" ;;
        *) SUITE_CASE_LABELS[$idx]="items" ;;
    esac
    rm -f "$logfile"
    emit_result "$idx"
}

skip_test() {
    local idx="$1"
    local name="$2"
    local role="$3"
    local reason="$4"
    echo ""
    echo "════════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════════"
    echo "  Role: $role"
    echo -e "${YELLOW}⚠ SKIPPED: ${reason}${NC}"
    SUITE_NAMES[$idx]="$name"
    SUITE_ROLES[$idx]="$role"
    SUITE_STATUS[$idx]="SKIP"
    SUITE_CASES[$idx]="0"
    SUITE_CASE_LABELS[$idx]="cases"
    emit_result "$idx"
}

start_servers() {
    echo ""
    echo "Starting servers..."
    
    # Kill any existing servers first
    pkill -f "sync_server 50051" 2>/dev/null
    pkill -f "json_server localhost:50051" 2>/dev/null
    sleep 1
    
    # Start gRPC server
    if [ -f "${BUILD}/server/sync_server" ]; then
        ${BUILD}/server/sync_server 50051 > /tmp/grpc.log 2>&1 &
        GRPC_PID=$!
        sleep 2
        
        # Verify gRPC server is running
        if ! kill -0 $GRPC_PID 2>/dev/null; then
            echo -e "${RED}ERROR: gRPC server failed to start. Check /tmp/grpc.log${NC}"
            cat /tmp/grpc.log
            return 1
        fi
        echo "  gRPC server started (PID: $GRPC_PID)"
    else
        echo -e "${RED}ERROR: sync_server binary not found${NC}"
        return 1
    fi
    
    # Start JSON server
    if [ -f "${BUILD}/jsonserver/json_server" ]; then
        ${BUILD}/jsonserver/json_server localhost:50051 8080 > /tmp/json.log 2>&1 &
        JSON_PID=$!
        sleep 2
        
        # Verify JSON server is running
        if ! kill -0 $JSON_PID 2>/dev/null; then
            echo -e "${RED}ERROR: JSON server failed to start. Check /tmp/json.log${NC}"
            cat /tmp/json.log
            return 1
        fi
        echo "  JSON server started (PID: $JSON_PID)"
    else
        echo -e "${RED}ERROR: json_server binary not found${NC}"
        return 1
    fi
    
    # Verify JSON server health endpoint
    sleep 1
    if ! curl -s http://localhost:8080/health > /dev/null 2>&1; then
        echo -e "${RED}ERROR: JSON server health check failed${NC}"
        return 1
    fi
    echo "  Health check passed"

    return 0
}

# Start a second gRPC+JSON server pair with the curve cache ENABLED, on distinct
# ports (gRPC 50052, HTTP 8081), for Suite 6's cache-OFF vs cache-ON comparison.
# The cache lives in the pricing engine (the gRPC server), so the cache env vars
# and the resulting CurveCache hit/miss log lines belong to the gRPC process.
start_cache_servers() {
    echo ""
    echo "Starting cache-ON servers (curve cache enabled)..."

    pkill -f "sync_server 50052" 2>/dev/null
    pkill -f "json_server localhost:50052" 2>/dev/null
    sleep 1

    if [ ! -f "${BUILD}/server/sync_server" ] || [ ! -f "${BUILD}/jsonserver/json_server" ]; then
        echo -e "${RED}ERROR: server binaries not found${NC}"
        return 1
    fi

    # gRPC engine: cache enabled + hit/miss logging to /tmp/grpc_cache.log (the
    # file Suite 6 reads to prove the cache engaged).
    QUANTRA_CURVE_CACHE_ENABLED=1 QUANTRA_CURVE_CACHE_LOG=1 \
        ${BUILD}/server/sync_server 50052 > /tmp/grpc_cache.log 2>&1 &
    GRPC_CACHE_PID=$!
    sleep 2
    if ! kill -0 $GRPC_CACHE_PID 2>/dev/null; then
        echo -e "${RED}ERROR: cache-ON gRPC server failed to start. Check /tmp/grpc_cache.log${NC}"
        cat /tmp/grpc_cache.log
        return 1
    fi
    echo "  cache-ON gRPC server started (PID: $GRPC_CACHE_PID, port 50052)"

    # JSON gateway pointed at the cache-ON gRPC engine.
    ${BUILD}/jsonserver/json_server localhost:50052 8081 > /tmp/json_cache.log 2>&1 &
    JSON_CACHE_PID=$!
    sleep 2
    if ! kill -0 $JSON_CACHE_PID 2>/dev/null; then
        echo -e "${RED}ERROR: cache-ON JSON server failed to start. Check /tmp/json_cache.log${NC}"
        cat /tmp/json_cache.log
        return 1
    fi
    echo "  cache-ON JSON server started (PID: $JSON_CACHE_PID, port 8081)"

    sleep 1
    if ! curl -s http://localhost:8081/health > /dev/null 2>&1; then
        echo -e "${RED}ERROR: cache-ON JSON server health check failed${NC}"
        return 1
    fi
    echo "  cache-ON health check passed"

    return 0
}

echo ""
echo "╔════════════════════════════════════════════╗"
echo "║         QUANTRA TEST SUITE                 ║"
echo "╚════════════════════════════════════════════╝"
echo ""
echo "Workspace: ${WORKSPACE}"
echo "Build:     ${BUILD}"

# Suite 0: pricer boundary guard. Static grep over parser/*_pricer.{h,cpp};
# runs before any server starts because no runtime is involved. A red
# Suite 0 must fail the whole runner — same rc-based pattern as the rest.
if [ -x "${WORKSPACE}/scripts/check_pricer_boundary.sh" ]; then
    run_test 0 \
        "0. Pricer boundary" \
        "Static guard: no FlatBuffers/gRPC in parser/*_pricer.{h,cpp}" \
        boundary \
        "${WORKSPACE}/scripts/check_pricer_boundary.sh"
else
    skip_test 0 \
        "0. Pricer boundary" \
        "Static guard: no FlatBuffers/gRPC in parser/*_pricer.{h,cpp}" \
        "scripts/check_pricer_boundary.sh not found"
    ((FAILED++))
fi

# Start servers
if ! start_servers; then
    echo -e "${RED}Failed to start servers. Aborting.${NC}"
    exit 1
fi

# Test 1: C++ authoritative pricing correctness
if [ -f "${BUILD}/tests/test_quantra_vs_quantlib" ]; then
    run_test 1 \
        "1. C++ QuantLib Parity" \
        "Authoritative pricing correctness against QuantLib" \
        gtest \
        "${BUILD}/tests/test_quantra_vs_quantlib"
else
    skip_test 1 \
        "1. C++ QuantLib Parity" \
        "Authoritative pricing correctness against QuantLib" \
        "Binary not found"
    ((FAILED++))
fi

# Check if gRPC server is still alive after unit tests
if ! kill -0 $GRPC_PID 2>/dev/null; then
    echo -e "${RED}WARNING: gRPC server crashed during unit tests. Restarting...${NC}"
    cat /tmp/grpc.log | tail -20
    start_servers
fi

# Test 2: C++ gRPC integration
if [ -f "${BUILD}/tests/test_server_client" ]; then
    run_test 2 \
        "2. C++ gRPC Integration" \
        "Transport/server round-trip coverage with representative product paths" \
        gtest \
        "${BUILD}/tests/test_server_client"
else
    skip_test 2 \
        "2. C++ gRPC Integration" \
        "Transport/server round-trip coverage with representative product paths" \
        "Binary not found"
    ((FAILED++))
fi

# Check if gRPC server is still alive
if ! kill -0 $GRPC_PID 2>/dev/null; then
    echo -e "${RED}WARNING: gRPC server crashed. Restarting...${NC}"
    cat /tmp/grpc.log | tail -20
    start_servers
fi

# Test 3: JSON API contract + representative parity (pytest, tests/contract/)
CONTRACT_DIR="${SCRIPT_DIR}/contract"
if [ -d "$CONTRACT_DIR" ]; then
    if curl -s http://localhost:8080/health > /dev/null 2>&1; then
        run_test 3 \
            "3. JSON HTTP API" \
            "HTTP contract coverage plus representative end-to-end parity scenarios" \
            pytest \
            "cd ${WORKSPACE} && python3 -m pytest ${CONTRACT_DIR} --url http://localhost:8080 --data-dir ${WORKSPACE}/examples/data -q"
    else
        skip_test 3 \
            "3. JSON HTTP API" \
            "HTTP contract coverage plus representative end-to-end parity scenarios" \
            "JSON server not running"
        ((FAILED++))
    fi
else
    skip_test 3 \
        "3. JSON HTTP API" \
        "HTTP contract coverage plus representative end-to-end parity scenarios" \
        "Contract test dir not found"
    ((FAILED++))
fi

# Check if gRPC server is still alive
if ! kill -0 $GRPC_PID 2>/dev/null; then
    echo -e "${RED}WARNING: gRPC server crashed. Restarting...${NC}"
    cat /tmp/grpc.log | tail -20
    start_servers
fi

# Test 4: Python client surface
PYTHON_TEST="${SCRIPT_DIR}/test_python_client.py"
if [ -f "$PYTHON_TEST" ]; then
    run_test 4 \
        "4. Python gRPC Client" \
        "Client binding usability with representative pricing scenarios" \
        python \
        "cd /tmp && PYTHONPATH=${WORKSPACE}/quantra-python:${WORKSPACE}/flatbuffers/python python3 ${PYTHON_TEST}"
else
    skip_test 4 \
        "4. Python gRPC Client" \
        "Client binding usability with representative pricing scenarios" \
        "Test file not found"
    ((FAILED++))
fi

# Test 5: JSON gateway concurrency / parser thread-safety
CONCURRENCY_TEST="${SCRIPT_DIR}/test_json_concurrency.py"
if [ -f "$CONCURRENCY_TEST" ]; then
    if curl -s http://localhost:8080/health > /dev/null 2>&1; then
        run_test 5 \
            "5. JSON Concurrency" \
            "Gateway thread-safety: many concurrent requests to one endpoint return identical results" \
            concurrency \
            "python3 ${CONCURRENCY_TEST} --url http://localhost:8080 --data-dir ${WORKSPACE}/examples/data"
    else
        skip_test 5 \
            "5. JSON Concurrency" \
            "Gateway thread-safety under concurrent load" \
            "JSON server not running"
        ((FAILED++))
    fi
else
    skip_test 5 \
        "5. JSON Concurrency" \
        "Gateway thread-safety under concurrent load" \
        "Test file not found"
    ((FAILED++))
fi

# Test 6: Curve cache correctness (transparency) — cache-OFF vs cache-ON
CACHING_DIR="${SCRIPT_DIR}/caching"
if [ -d "$CACHING_DIR" ]; then
    if ! curl -s http://localhost:8080/health > /dev/null 2>&1; then
        skip_test 6 \
            "6. Curve Cache Correctness" \
            "Cache transparency: cache-OFF and cache-ON results are bit-for-bit identical" \
            "cache-OFF JSON server not running"
        ((FAILED++))
    elif ! start_cache_servers; then
        echo -e "${RED}Failed to start cache-ON servers.${NC}"
        skip_test 6 \
            "6. Curve Cache Correctness" \
            "Cache transparency: cache-OFF and cache-ON results are bit-for-bit identical" \
            "cache-ON servers failed to start"
        ((FAILED++))
    else
        run_test 6 \
            "6. Curve Cache Correctness" \
            "Cache transparency: cache-OFF and cache-ON results are bit-for-bit identical (gated)" \
            cache \
            "cd ${WORKSPACE} && python3 -m pytest ${CACHING_DIR} --url-nocache http://localhost:8080 --url-cache http://localhost:8081 --data-dir ${WORKSPACE}/examples/data --cache-log /tmp/grpc_cache.log -q -s"
    fi
else
    skip_test 6 \
        "6. Curve Cache Correctness" \
        "Cache transparency: cache-OFF and cache-ON results are bit-for-bit identical" \
        "Caching test dir not found"
    ((FAILED++))
fi

# Summary
TOTAL=$((PASSED + FAILED))
TOTAL_CASES=0
KNOWN_CASES=1
for idx in 0 1 2 3 4 5 6; do
    if [[ "${SUITE_CASES[$idx]}" =~ ^[0-9]+$ ]]; then
        TOTAL_CASES=$((TOTAL_CASES + SUITE_CASES[$idx]))
    else
        KNOWN_CASES=0
    fi
done
echo ""
echo "╔════════════════════════════════════════════╗"
echo "║          TEST SUITE SUMMARY                ║"
echo "╠════════════════════════════════════════════╣"
printf "║  Suite groups passed: %-22s║\n" "${PASSED}/${TOTAL}"
printf "║  Suite groups failed: %-22s║\n" "${FAILED}"
if [ "$KNOWN_CASES" -eq 1 ]; then
    printf "║  Underlying cases/scenarios: %-18s║\n" "${TOTAL_CASES}"
else
    printf "║  Underlying cases/scenarios: %-18s║\n" "mixed/unknown"
fi
echo "╚════════════════════════════════════════════╝"
echo ""
printf "%-28s %-8s %-12s %s\n" "Suite" "Status" "Coverage" "Role"
printf "%-28s %-8s %-12s %s\n" "-----" "------" "--------" "----"
for idx in 0 1 2 3 4 5 6; do
    printf "%-28s %-8s %-12s %s\n" \
        "${SUITE_NAMES[$idx]}" \
        "${SUITE_STATUS[$idx]}" \
        "${SUITE_CASES[$idx]} ${SUITE_CASE_LABELS[$idx]}" \
        "${SUITE_ROLES[$idx]}"
done
echo ""

# Machine-readable aggregate (mirrors the box above). Greppable verbatim via
# `grep '^SUMMARY '`; the per-suite RESULT lines were emitted as each suite ran.
printf 'SUMMARY suites_passed=%s suites_failed=%s total_cases=%s\n' \
    "${PASSED}" "${FAILED}" "${TOTAL_CASES}"
echo ""

if [ "$FAILED" -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    exit 1
fi