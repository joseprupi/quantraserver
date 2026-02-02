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

run_test() {
    local name="$1"
    local cmd="$2"
    echo ""
    echo "════════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════════"
    if eval "$cmd"; then
        echo -e "${GREEN}✓ PASSED${NC}"
        ((PASSED++))
    else
        echo -e "${RED}✗ FAILED${NC}"
        ((FAILED++))
    fi
}

skip_test() {
    local name="$1"
    local reason="$2"
    echo ""
    echo "════════════════════════════════════════════"
    echo "  $name"
    echo "════════════════════════════════════════════"
    echo -e "${YELLOW}⚠ SKIPPED: ${reason}${NC}"
}

echo ""
echo "╔════════════════════════════════════════════╗"
echo "║         QUANTRA TEST SUITE                 ║"
echo "╚════════════════════════════════════════════╝"
echo ""
echo "Workspace: ${WORKSPACE}"
echo "Build:     ${BUILD}"

# Start servers if not running
if ! curl -s http://localhost:8080/health > /dev/null 2>&1; then
    echo ""
    echo "Starting servers..."
    if [ -f "${BUILD}/server/sync_server" ]; then
        ${BUILD}/server/sync_server 50051 > /tmp/grpc.log 2>&1 &
        sleep 2
    fi
    if [ -f "${BUILD}/jsonserver/json_server" ]; then
        ${BUILD}/jsonserver/json_server localhost:50051 8080 > /tmp/json.log 2>&1 &
        sleep 2
    fi
fi

# Test 1: C++ Unit Tests
if [ -f "${BUILD}/tests/test_quantra_vs_quantlib" ]; then
    run_test "1. C++ Unit Tests (Quantra vs QuantLib)" \
        "${BUILD}/tests/test_quantra_vs_quantlib"
else
    skip_test "1. C++ Unit Tests" "Binary not found"
    ((FAILED++))
fi

# Test 2: C++ gRPC Integration
if [ -f "${BUILD}/tests/test_server_client" ]; then
    run_test "2. C++ gRPC Integration Tests" \
        "${BUILD}/tests/test_server_client"
else
    skip_test "2. C++ gRPC Integration Tests" "Binary not found"
    ((FAILED++))
fi

# Test 3: JSON API Tests
JSON_TEST=""
for f in test_json_api_vs_quantlib.py test_json_files_vs_quantlib.py; do
    [ -f "${SCRIPT_DIR}/${f}" ] && JSON_TEST="${SCRIPT_DIR}/${f}" && break
done

if [ -n "$JSON_TEST" ]; then
    if curl -s http://localhost:8080/health > /dev/null 2>&1; then
        run_test "3. JSON HTTP API Tests" \
            "python3 ${JSON_TEST} --url http://localhost:8080 --data-dir ${WORKSPACE}/examples/data"
    else
        skip_test "3. JSON HTTP API Tests" "JSON server not running"
        ((FAILED++))
    fi
else
    skip_test "3. JSON HTTP API Tests" "Test file not found"
    ((FAILED++))
fi

# Test 4: Python gRPC Client
PYTHON_TEST="${SCRIPT_DIR}/test_python_client.py"
if [ -f "$PYTHON_TEST" ]; then
    run_test "4. Python gRPC Client Tests" \
        "cd /tmp && PYTHONPATH=${WORKSPACE}/quantra-python:${WORKSPACE}/flatbuffers/python python3 ${PYTHON_TEST}"
else
    skip_test "4. Python gRPC Client Tests" "Test file not found"
    ((FAILED++))
fi

# Summary
TOTAL=$((PASSED + FAILED))
echo ""
echo "╔════════════════════════════════════════════╗"
echo "║              TEST SUMMARY                  ║"
echo "╠════════════════════════════════════════════╣"
printf "║  Passed: %-33s║\n" "${PASSED}"
printf "║  Failed: %-33s║\n" "${FAILED}"
printf "║  Total:  %-33s║\n" "${TOTAL}"
echo "╚════════════════════════════════════════════╝"
echo ""

if [ "$FAILED" -eq 0 ]; then
    echo -e "${GREEN}✓ ALL TESTS PASSED!${NC}"
    exit 0
else
    echo -e "${RED}✗ SOME TESTS FAILED${NC}"
    exit 1
fi