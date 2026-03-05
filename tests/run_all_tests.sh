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
    # Also kill any orphaned servers
    pkill -f "sync_server 50051" 2>/dev/null
    pkill -f "json_server localhost:50051" 2>/dev/null
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
        python)
            awk '/^Total scenarios:/ {split($3, a, "/"); total=a[2]} END {if (total == "") total="?"; print total}' "$logfile"
            ;;
        *)
            echo "?"
            ;;
    esac
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
    if bash -lc "$cmd" 2>&1 | tee "$logfile"; then
        echo -e "${GREEN}✓ PASSED${NC}"
        SUITE_STATUS[$idx]="PASS"
        ((PASSED++))
    else
        echo -e "${RED}✗ FAILED${NC}"
        SUITE_STATUS[$idx]="FAIL"
        ((FAILED++))
    fi
    SUITE_NAMES[$idx]="$name"
    SUITE_ROLES[$idx]="$role"
    SUITE_CASES[$idx]="$(extract_case_count "$kind" "$logfile")"
    case "$kind" in
        gtest) SUITE_CASE_LABELS[$idx]="cases" ;;
        json) SUITE_CASE_LABELS[$idx]="scenarios" ;;
        python) SUITE_CASE_LABELS[$idx]="scenarios" ;;
        *) SUITE_CASE_LABELS[$idx]="items" ;;
    esac
    rm -f "$logfile"
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

echo ""
echo "╔════════════════════════════════════════════╗"
echo "║         QUANTRA TEST SUITE                 ║"
echo "╚════════════════════════════════════════════╝"
echo ""
echo "Workspace: ${WORKSPACE}"
echo "Build:     ${BUILD}"

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

# Test 3: JSON API contract + representative parity
JSON_TEST=""
for f in test_json_api_vs_quantlib.py test_json_files_vs_quantlib.py; do
    [ -f "${SCRIPT_DIR}/${f}" ] && JSON_TEST="${SCRIPT_DIR}/${f}" && break
done

if [ -n "$JSON_TEST" ]; then
    if curl -s http://localhost:8080/health > /dev/null 2>&1; then
        run_test 3 \
            "3. JSON HTTP API" \
            "HTTP contract coverage plus representative end-to-end parity scenarios" \
            json \
            "python3 ${JSON_TEST} --url http://localhost:8080 --data-dir ${WORKSPACE}/examples/data"
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
        "Test file not found"
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

# Summary
TOTAL=$((PASSED + FAILED))
TOTAL_CASES=0
KNOWN_CASES=1
for idx in 1 2 3 4; do
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
for idx in 1 2 3 4; do
    printf "%-28s %-8s %-12s %s\n" \
        "${SUITE_NAMES[$idx]}" \
        "${SUITE_STATUS[$idx]}" \
        "${SUITE_CASES[$idx]} ${SUITE_CASE_LABELS[$idx]}" \
        "${SUITE_ROLES[$idx]}"
done
echo ""

if [ "$FAILED" -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    exit 1
fi