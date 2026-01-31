#!/bin/bash
set -e

# Define paths
WORKSPACE="/workspace"
FBS_DIR="$WORKSPACE/flatbuffers/fbs"
GEN_PYTHON_DIR="$WORKSPACE/flatbuffers/python"
GEN_CPP_DIR="$WORKSPACE/flatbuffers/cpp"

echo "========================================"
echo "   Building Quantra Protocol Buffers"
echo "========================================"

# 1. Clean generated code
echo "[1/5] Cleaning generated files..."
rm -rf "$GEN_PYTHON_DIR/quantra"
mkdir -p "$GEN_PYTHON_DIR"
mkdir -p "$GEN_CPP_DIR"

# 2. Generate C++
echo "[2/5] Generating C++ code..."
flatc --cpp --gen-all -o "$GEN_CPP_DIR" "$FBS_DIR"/*.fbs

# 3. Generate Python
echo "[3/5] Generating Python code..."
flatc --python --gen-object-api --gen-all -o "$GEN_PYTHON_DIR" "$FBS_DIR"/*.fbs

# 4. Create Package
echo "[4/5] Finalizing package..."
touch "$GEN_PYTHON_DIR/quantra/__init__.py"

# 5. [CRITICAL FIX] Patch the broken generated file
# The generated PriceFixedRateBondResponse.py tries to use FixedRateBondResponse
# without importing it. We inject the import statement into the Bonds method.
echo "[5/5] Patching generated code imports..."

TARGET_FILE="$GEN_PYTHON_DIR/quantra/PriceFixedRateBondResponse.py"

if [ -f "$TARGET_FILE" ]; then
    # Use sed to find the 'def Bonds' line and append the import after it
    # We use a temp file to ensure safety
    sed '/def Bonds(self, j):/a \        from quantra.FixedRateBondResponse import FixedRateBondResponse' "$TARGET_FILE" > "${TARGET_FILE}.tmp" && mv "${TARGET_FILE}.tmp" "$TARGET_FILE"
    echo " -> Patched PriceFixedRateBondResponse.py"
else
    echo "WARNING: Could not find file to patch: $TARGET_FILE"
fi

echo "Done! Generated code is in $GEN_PYTHON_DIR/quantra"