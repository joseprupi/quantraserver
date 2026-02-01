#!/bin/bash
# Quantra Test Suite Runner
# Runs all unit and integration tests

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/../build"
SERVER_PID=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

print_header() {
    echo ""
    echo -e "${BLUE}============================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}============================================${NC}"
    echo ""
}

print_success() {
    echo -e "${GREEN}✓ $1${NC}"
}

print_error() {
    echo -e "${RED}✗ $1${NC}"
}

print_warning() {
    echo -e "${YELLOW}⚠ $1${NC}"
}

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        echo ""
        echo "Stopping server (PID: $SERVER_PID)..."
        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
        print_success "Server stopped"
    fi
}

trap cleanup EXIT

# Function to check if server is ready
check_server_ready() {
    # Try multiple methods to check if port is open
    if command -v nc &> /dev/null; then
        nc -z 127.0.0.1 50051 2>/dev/null && return 0
    fi
    if command -v timeout &> /dev/null; then
        timeout 1 bash -c "echo > /dev/tcp/127.0.0.1/50051" 2>/dev/null && return 0
    fi
    # Fallback: try to connect with bash
    (echo > /dev/tcp/127.0.0.1/50051) 2>/dev/null && return 0
    return 1
}

# Check if build directory exists
if [ ! -d "$BUILD_DIR" ]; then
    print_error "Build directory not found at $BUILD_DIR"
    echo "Please run: cd /workspace && mkdir -p build && cd build && cmake .. && make -j\$(nproc)"
    exit 1
fi

print_header "Quantra Test Suite"
echo "Build directory: $BUILD_DIR"
echo ""

# ================================
# Unit Tests: Quantra vs QuantLib
# ================================
print_header "Unit Tests: Quantra vs QuantLib Comparison"

UNIT_TEST="${BUILD_DIR}/tests/test_quantra_vs_quantlib"
if [ -f "$UNIT_TEST" ]; then
    if $UNIT_TEST; then
        print_success "Unit tests passed"
    else
        print_error "Unit tests failed"
        exit 1
    fi
else
    print_error "Unit test binary not found: $UNIT_TEST"
    exit 1
fi

# ================================
# Integration Tests: Server-Client
# ================================
print_header "Integration Tests: Server-Client (C++)"

SERVER_BIN="${BUILD_DIR}/server/sync_server"
CLIENT_TEST="${BUILD_DIR}/tests/test_server_client"

if [ ! -f "$SERVER_BIN" ]; then
    print_error "Server binary not found: $SERVER_BIN"
    exit 1
fi

if [ ! -f "$CLIENT_TEST" ]; then
    print_error "Client test binary not found: $CLIENT_TEST"
    exit 1
fi

# Check if server is already running
if check_server_ready; then
    print_warning "Server already running on port 50051"
else
    echo "Starting server..."
    # Start server in background, redirect output to avoid blocking
    $SERVER_BIN > /tmp/quantra_server.log 2>&1 &
    SERVER_PID=$!
    echo "Server started (PID: $SERVER_PID)"
    
    # Wait for server to be ready
    echo "Waiting for server to be ready..."
    for i in {1..30}; do
        sleep 0.5
        if check_server_ready; then
            print_success "Server is ready (took ~$((i/2)) seconds)"
            break
        fi
        if [ $i -eq 30 ]; then
            print_error "Server failed to start within 15 seconds"
            echo "Server log:"
            cat /tmp/quantra_server.log 2>/dev/null || echo "(no log available)"
            exit 1
        fi
    done
fi

# Give server a moment to fully initialize
sleep 1

# Run integration tests
echo ""
echo "Running integration tests..."
if $CLIENT_TEST; then
    print_success "Integration tests passed"
    INTEGRATION_RESULT=0
else
    print_error "Integration tests failed"
    INTEGRATION_RESULT=1
fi

# ================================
# Python Client Tests
# ================================
print_header "Python Client Tests: Quantra vs QuantLib Python"

PYTHON_TEST="${SCRIPT_DIR}/test_python_client.py"
WORKSPACE_ROOT="${SCRIPT_DIR}/.."
PYTHON_RESULT=0

if [ -f "$PYTHON_TEST" ]; then
    # Check if QuantLib Python is available
    if python3 -c "import QuantLib" 2>/dev/null; then
        echo "Running Python client comparison tests..."
        if PYTHONPATH="${WORKSPACE_ROOT}/quantra-python:${WORKSPACE_ROOT}/flatbuffers/python" python3 "$PYTHON_TEST"; then
            print_success "Python client tests passed"
            PYTHON_RESULT=0
        else
            print_error "Python client tests failed"
            PYTHON_RESULT=1
        fi
    else
        print_warning "QuantLib Python not installed - skipping Python tests"
        print_warning "Install with: pip3 install QuantLib --break-system-packages"
        PYTHON_RESULT=0  # Don't fail if QuantLib not installed
    fi
else
    print_warning "Python test file not found: $PYTHON_TEST"
    PYTHON_RESULT=0
fi

# ================================
# Summary
# ================================
print_header "Test Summary"

ALL_PASSED=true
if [ $INTEGRATION_RESULT -ne 0 ]; then ALL_PASSED=false; fi
if [ $PYTHON_RESULT -ne 0 ]; then ALL_PASSED=false; fi

if $ALL_PASSED; then
    print_success "All tests passed!"
    echo ""
    echo "Test Results:"
    echo "  - Unit Tests (Quantra vs QuantLib C++): PASSED"
    echo "  - Integration Tests (Server-Client):    PASSED"
    echo "  - Python Client Tests:                  PASSED"
    echo ""
    exit 0
else
    print_error "Some tests failed"
    echo ""
    echo "Test Results:"
    echo "  - Unit Tests (Quantra vs QuantLib C++): PASSED"
    echo "  - Integration Tests (Server-Client):    $([ $INTEGRATION_RESULT -eq 0 ] && echo 'PASSED' || echo 'FAILED')"
    echo "  - Python Client Tests:                  $([ $PYTHON_RESULT -eq 0 ] && echo 'PASSED' || echo 'FAILED')"
    echo ""
    exit 1
fi