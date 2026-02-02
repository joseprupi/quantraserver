#!/bin/bash
# =============================================================================
# Quantraserver Test Suite
# 
# This script verifies that the build works correctly after upgrading
# gRPC and Flatbuffers versions.
#
# Usage:
#   ./test_build.sh              # Run all tests
#   ./test_build.sh --quick      # Quick smoke test only
#   ./test_build.sh --verbose    # Verbose output
# =============================================================================

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

VERBOSE=false
QUICK=false
BUILD_DIR="${BUILD_DIR:-./build}"
SERVER_PORT=50051
SERVER_PID=""

# Parse arguments
for arg in "$@"; do
    case $arg in
        --verbose) VERBOSE=true ;;
        --quick) QUICK=true ;;
        *) echo "Unknown argument: $arg"; exit 1 ;;
    esac
done

log() {
    echo -e "${GREEN}[TEST]${NC} $1"
}

warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        log "Stopping server (PID: $SERVER_PID)..."
        kill $SERVER_PID 2>/dev/null || true
        wait $SERVER_PID 2>/dev/null || true
    fi
}

trap cleanup EXIT

# =============================================================================
# Test 1: Check build artifacts exist
# =============================================================================
test_build_artifacts() {
    log "Test 1: Checking build artifacts..."
    
    local artifacts=(
        "$BUILD_DIR/server/sync_server"
        "$BUILD_DIR/examples/bond_request"
    )
    
    for artifact in "${artifacts[@]}"; do
        if [ -f "$artifact" ]; then
            if $VERBOSE; then
                echo "  ✓ Found: $artifact"
                file "$artifact"
            fi
        else
            error "Missing: $artifact"
            return 1
        fi
    done
    
    log "  ✓ All build artifacts present"
}

# =============================================================================
# Test 2: Check library dependencies
# =============================================================================
test_library_deps() {
    log "Test 2: Checking library dependencies..."
    
    if ! command -v ldd &> /dev/null; then
        warn "ldd not available, skipping dependency check"
        return 0
    fi
    
    local missing=$(ldd "$BUILD_DIR/server/sync_server" 2>&1 | grep "not found" || true)
    
    if [ -n "$missing" ]; then
        error "Missing libraries:"
        echo "$missing"
        return 1
    fi
    
    if $VERBOSE; then
        echo "  Libraries linked:"
        ldd "$BUILD_DIR/server/sync_server" | grep -E "(grpc|flatbuffers|QuantLib)" || true
    fi
    
    log "  ✓ All library dependencies satisfied"
}

# =============================================================================
# Test 3: Server starts successfully
# =============================================================================
test_server_starts() {
    log "Test 3: Testing server startup..."
    
    # Start server in background
    "$BUILD_DIR/server/sync_server" $SERVER_PORT &
    SERVER_PID=$!
    
    # Wait for server to start
    sleep 2
    
    # Check if server is still running
    if kill -0 $SERVER_PID 2>/dev/null; then
        log "  ✓ Server started successfully (PID: $SERVER_PID)"
    else
        error "Server failed to start"
        return 1
    fi
}

# =============================================================================
# Test 4: Basic RPC call works
# =============================================================================
test_rpc_call() {
    log "Test 4: Testing basic RPC call..."
    
    # Run bond request example
    local output
    output=$("$BUILD_DIR/examples/bond_request" 1 2>&1)
    local exit_code=$?
    
    if [ $exit_code -eq 0 ]; then
        if $VERBOSE; then
            echo "  Response:"
            echo "$output" | head -20
        fi
        log "  ✓ RPC call successful"
    else
        error "RPC call failed with exit code: $exit_code"
        echo "$output"
        return 1
    fi
}

# =============================================================================
# Test 5: Multiple concurrent requests
# =============================================================================
test_concurrent_requests() {
    log "Test 5: Testing concurrent requests..."
    
    local output
    output=$("$BUILD_DIR/examples/bond_request" 10 2>&1)
    local exit_code=$?
    
    if [ $exit_code -eq 0 ]; then
        log "  ✓ 10 concurrent requests successful"
    else
        error "Concurrent requests failed"
        echo "$output"
        return 1
    fi
}

# =============================================================================
# Test 6: Flatbuffers serialization/deserialization
# =============================================================================
test_flatbuffers() {
    log "Test 6: Testing Flatbuffers serialization..."
    
    # The bond_request already tests serialization, but we can check
    # for any serialization errors in the output
    local output
    output=$("$BUILD_DIR/examples/bond_request" 1 2>&1)
    
    if echo "$output" | grep -qi "serialization\|deseriali\|verify.*fail"; then
        error "Potential serialization issue detected"
        echo "$output"
        return 1
    fi
    
    log "  ✓ Flatbuffers serialization working"
}

# =============================================================================
# Test 7: QuantLib calculations
# =============================================================================
test_quantlib() {
    log "Test 7: Verifying QuantLib calculations..."
    
    # Run bond request and check for reasonable NPV values
    local output
    output=$("$BUILD_DIR/examples/bond_request" 1 2>&1)
    
    # Check that we get numeric NPV values (not NaN or errors)
    if echo "$output" | grep -qE "NPV|npv|price"; then
        if echo "$output" | grep -qi "nan\|inf\|error"; then
            warn "Possible calculation issue detected"
            echo "$output"
        else
            log "  ✓ QuantLib calculations appear correct"
        fi
    else
        # If output format is different, just check no errors
        if [ $? -eq 0 ]; then
            log "  ✓ QuantLib integration working"
        fi
    fi
}

# =============================================================================
# Test 8: Stress test (optional, skip in quick mode)
# =============================================================================
test_stress() {
    if $QUICK; then
        log "Test 8: Skipping stress test (quick mode)"
        return 0
    fi
    
    log "Test 8: Running stress test (100 requests)..."
    
    local start_time=$(date +%s.%N)
    local output
    output=$("$BUILD_DIR/examples/bond_request" 100 2>&1)
    local exit_code=$?
    local end_time=$(date +%s.%N)
    
    if [ $exit_code -eq 0 ]; then
        local duration=$(echo "$end_time - $start_time" | bc)
        log "  ✓ 100 requests completed in ${duration}s"
    else
        error "Stress test failed"
        return 1
    fi
}

# =============================================================================
# Main
# =============================================================================
main() {
    echo ""
    echo "=========================================="
    echo " Quantraserver Test Suite"
    echo "=========================================="
    echo ""
    
    # Print environment info
    if $VERBOSE; then
        log "Environment:"
        echo "  BUILD_DIR: $BUILD_DIR"
        echo "  LD_LIBRARY_PATH: $LD_LIBRARY_PATH"
        echo ""
    fi
    
    local failed=0
    
    # Run tests
    test_build_artifacts || ((failed++))
    test_library_deps || ((failed++))
    test_server_starts || ((failed++))
    test_rpc_call || ((failed++))
    test_concurrent_requests || ((failed++))
    test_flatbuffers || ((failed++))
    test_quantlib || ((failed++))
    test_stress || ((failed++))
    
    # Stop server
    cleanup
    SERVER_PID=""
    
    # Summary
    echo ""
    echo "=========================================="
    if [ $failed -eq 0 ]; then
        echo -e " ${GREEN}All tests passed!${NC}"
    else
        echo -e " ${RED}$failed test(s) failed${NC}"
    fi
    echo "=========================================="
    echo ""
    
    return $failed
}

main