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

# Test 1: C++ Unit Tests
if [ -f "${BUILD}/tests/test_quantra_vs_quantlib" ]; then
    run_test "1. C++ Unit Tests (Quantra vs QuantLib)" \
        "${BUILD}/tests/test_quantra_vs_quantlib"
else
    skip_test "1. C++ Unit Tests" "Binary not found"
    ((FAILED++))
fi

# Check if gRPC server is still alive after unit tests
if ! kill -0 $GRPC_PID 2>/dev/null; then
    echo -e "${RED}WARNING: gRPC server crashed during unit tests. Restarting...${NC}"
    cat /tmp/grpc.log | tail -20
    start_servers
fi

# Test 2: C++ gRPC Integration
if [ -f "${BUILD}/tests/test_server_client" ]; then
    run_test "2. C++ gRPC Integration Tests" \
        "${BUILD}/tests/test_server_client"
else
    skip_test "2. C++ gRPC Integration Tests" "Binary not found"
    ((FAILED++))
fi

# Check if gRPC server is still alive
if ! kill -0 $GRPC_PID 2>/dev/null; then
    echo -e "${RED}WARNING: gRPC server crashed. Restarting...${NC}"
    cat /tmp/grpc.log | tail -20
    start_servers
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

# Check if gRPC server is still alive
if ! kill -0 $GRPC_PID 2>/dev/null; then
    echo -e "${RED}WARNING: gRPC server crashed. Restarting...${NC}"
    cat /tmp/grpc.log | tail -20
    start_servers
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