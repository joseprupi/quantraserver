#!/bin/bash
# Apply all schema refactoring changes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="${1:-/workspace}"

echo "=== Applying Schema Refactoring Changes ==="
echo "Source: $SCRIPT_DIR"
echo "Target: $WORKSPACE"
echo ""

# Check workspace exists
if [ ! -d "$WORKSPACE" ]; then
    echo "ERROR: Workspace not found at $WORKSPACE"
    exit 1
fi

# Step 1: Copy FBS files
echo "Step 1: Copying FBS schema files..."
cp "$SCRIPT_DIR/fbs/"*.fbs "$WORKSPACE/flatbuffers/fbs/"
echo "  ✓ Copied $(ls -1 $SCRIPT_DIR/fbs/*.fbs | wc -l) schema files"

# Step 2: Regenerate C++ headers
echo ""
echo "Step 2: Regenerating C++ headers from FlatBuffers..."
cd "$WORKSPACE/flatbuffers"
if [ -f "generate.sh" ]; then
    bash generate.sh
    echo "  ✓ Generated C++ headers"
else
    echo "  ⚠ generate.sh not found, trying flatc directly..."
    # Find flatc
    FLATC=$(which flatc 2>/dev/null || find /usr -name "flatc" -type f 2>/dev/null | head -1)
    if [ -z "$FLATC" ]; then
        echo "  ERROR: flatc not found"
        exit 1
    fi
    mkdir -p cpp
    for fbs in fbs/*.fbs; do
        $FLATC --cpp -o cpp -I fbs "$fbs"
    done
    echo "  ✓ Generated C++ headers"
fi

# Step 3: Copy parser files
echo ""
echo "Step 3: Copying updated parser files..."
cp "$SCRIPT_DIR/parser/"*.cpp "$WORKSPACE/parser/"
cp "$SCRIPT_DIR/parser/"*.h "$WORKSPACE/parser/"
echo "  ✓ Copied parser files"

# Step 4: Copy request handler files
echo ""
echo "Step 4: Copying updated request handler files..."
cp "$SCRIPT_DIR/request/"*.cpp "$WORKSPACE/request/"
echo "  ✓ Copied request handler files"

# Step 5: Copy test files
echo ""
echo "Step 5: Copying updated test files..."
cp "$SCRIPT_DIR/tests/"*.cpp "$WORKSPACE/tests/"
echo "  ✓ Copied test files"

# Step 6: Rebuild
echo ""
echo "Step 6: Rebuilding..."
cd "$WORKSPACE/build"
cmake .. > /dev/null
make -j$(nproc) 2>&1 | tail -20

echo ""
echo "=== Changes Applied ==="
echo ""
echo "Summary of changes:"
echo "  - All enums consolidated in enums.fbs"
echo "  - Volatility surfaces now in Pricing.volatilities (built once)"
echo "  - Cap/Floor and Swaption reference volatility by ID string"
echo "  - All enum usages updated to quantra::enums:: namespace"
echo ""
echo "Run tests with: $WORKSPACE/tests/run_all_tests.sh"
