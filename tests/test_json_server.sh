#!/bin/bash
# =============================================================================
# Quantra JSON Server Test Script
#
# Tests all endpoints of the JSON HTTP API server
#
# Usage:
#   ./test_json_server.sh [server_url]
#   ./test_json_server.sh http://localhost:8080
# =============================================================================

set -e

SERVER_URL="${1:-http://localhost:8080}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EXAMPLES_DIR="${SCRIPT_DIR}/../examples/data"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

print_header() {
    echo ""
    echo -e "${BLUE}============================================${NC}"
    echo -e "${BLUE}$1${NC}"
    echo -e "${BLUE}============================================${NC}"
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

# Check if curl is available
if ! command -v curl &> /dev/null; then
    print_error "curl is required but not installed"
    exit 1
fi

# Check if jq is available (optional, for pretty printing)
HAS_JQ=false
if command -v jq &> /dev/null; then
    HAS_JQ=true
fi

# Function to test an endpoint
test_endpoint() {
    local endpoint="$1"
    local json_file="$2"
    local description="$3"
    
    echo ""
    echo "Testing: $description"
    echo "  Endpoint: POST $endpoint"
    echo "  Input:    $json_file"
    
    if [ ! -f "$json_file" ]; then
        print_warning "JSON file not found: $json_file (skipping)"
        return 0
    fi
    
    # Make the request
    local response
    local http_code
    
    response=$(curl -s -w "\n%{http_code}" \
        -X POST \
        -H "Content-Type: application/json" \
        -d @"$json_file" \
        "${SERVER_URL}${endpoint}" 2>&1)
    
    # Extract HTTP code (last line)
    http_code=$(echo "$response" | tail -1)
    # Extract body (everything except last line)
    body=$(echo "$response" | sed '$d')
    
    if [ "$http_code" = "200" ]; then
        print_success "HTTP $http_code OK"
        
        # Show response (truncated if too long)
        if $HAS_JQ; then
            echo "  Response (formatted):"
            echo "$body" | jq '.' 2>/dev/null | head -20
        else
            echo "  Response:"
            echo "$body" | head -5
        fi
        
        # Check for NPV in response
        if echo "$body" | grep -q "npv"; then
            local npv=$(echo "$body" | grep -o '"npv":[^,}]*' | head -1)
            echo -e "  ${GREEN}Found: $npv${NC}"
        fi
        
        return 0
    else
        print_error "HTTP $http_code"
        echo "  Error: $body"
        return 1
    fi
}

# =============================================================================
# Main
# =============================================================================

print_header "Quantra JSON Server Tests"
echo "Server URL: $SERVER_URL"

# Test health endpoint
echo ""
echo "Testing: Health Check"
if curl -s "${SERVER_URL}/health" | grep -q "healthy"; then
    print_success "Server is healthy"
else
    print_error "Server health check failed"
    echo "Make sure the JSON server is running:"
    echo "  ./json_server localhost:50051 8080 /path/to/flatbuffers/fbs"
    exit 1
fi

PASSED=0
FAILED=0
SKIPPED=0

# =============================================================================
# Test Fixed Rate Bond
# =============================================================================
print_header "Fixed Rate Bond"

if test_endpoint "/price-fixed-rate-bond" \
    "${EXAMPLES_DIR}/fixed_rate_bond_request.json" \
    "Price Fixed Rate Bond"; then
    ((PASSED++))
else
    ((FAILED++))
fi

# =============================================================================
# Test Floating Rate Bond
# =============================================================================
print_header "Floating Rate Bond"

if [ -f "${EXAMPLES_DIR}/floating_rate_bond_request.json" ]; then
    if test_endpoint "/price-floating-rate-bond" \
        "${EXAMPLES_DIR}/floating_rate_bond_request.json" \
        "Price Floating Rate Bond"; then
        ((PASSED++))
    else
        ((FAILED++))
    fi
else
    print_warning "No floating rate bond example found (skipping)"
    ((SKIPPED++))
fi

# =============================================================================
# Test Vanilla Swap
# =============================================================================
print_header "Vanilla Swap"

if [ -f "${EXAMPLES_DIR}/vanilla_swap_request.json" ]; then
    if test_endpoint "/price-vanilla-swap" \
        "${EXAMPLES_DIR}/vanilla_swap_request.json" \
        "Price Vanilla Swap"; then
        ((PASSED++))
    else
        ((FAILED++))
    fi
else
    print_warning "No vanilla swap example found (skipping)"
    ((SKIPPED++))
fi

# =============================================================================
# Test FRA
# =============================================================================
print_header "FRA"

if [ -f "${EXAMPLES_DIR}/fra_request.json" ]; then
    if test_endpoint "/price-fra" \
        "${EXAMPLES_DIR}/fra_request.json" \
        "Price FRA"; then
        ((PASSED++))
    else
        ((FAILED++))
    fi
else
    print_warning "No FRA example found (skipping)"
    ((SKIPPED++))
fi

# =============================================================================
# Test Cap/Floor
# =============================================================================
print_header "Cap/Floor"

if [ -f "${EXAMPLES_DIR}/cap_floor_request.json" ]; then
    if test_endpoint "/price-cap-floor" \
        "${EXAMPLES_DIR}/cap_floor_request.json" \
        "Price Cap/Floor"; then
        ((PASSED++))
    else
        ((FAILED++))
    fi
else
    print_warning "No cap/floor example found (skipping)"
    ((SKIPPED++))
fi

# =============================================================================
# Test Swaption
# =============================================================================
print_header "Swaption"

if [ -f "${EXAMPLES_DIR}/swaption_request.json" ]; then
    if test_endpoint "/price-swaption" \
        "${EXAMPLES_DIR}/swaption_request.json" \
        "Price Swaption"; then
        ((PASSED++))
    else
        ((FAILED++))
    fi
else
    print_warning "No swaption example found (skipping)"
    ((SKIPPED++))
fi

# =============================================================================
# Test CDS
# =============================================================================
print_header "CDS"

if [ -f "${EXAMPLES_DIR}/cds_request.json" ]; then
    if test_endpoint "/price-cds" \
        "${EXAMPLES_DIR}/cds_request.json" \
        "Price CDS"; then
        ((PASSED++))
    else
        ((FAILED++))
    fi
else
    print_warning "No CDS example found (skipping)"
    ((SKIPPED++))
fi

# =============================================================================
# Summary
# =============================================================================
print_header "Summary"

echo "  Passed:  $PASSED"
echo "  Failed:  $FAILED"
echo "  Skipped: $SKIPPED"
echo ""

if [ $FAILED -eq 0 ]; then
    print_success "All tests passed!"
    exit 0
else
    print_error "$FAILED test(s) failed"
    exit 1
fi