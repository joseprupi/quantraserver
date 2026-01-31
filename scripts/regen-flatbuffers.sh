#!/bin/bash
set -e

# Define paths
WORKSPACE="/workspace"
FBS_DIR="$WORKSPACE/flatbuffers/fbs"
GEN_PYTHON_DIR="$WORKSPACE/flatbuffers/python"
GEN_CPP_DIR="$WORKSPACE/flatbuffers/cpp"
TOOLS_DIR="$WORKSPACE/quantra-python/tools"

echo "========================================"
echo "   Building Quantra Protocol Buffers"
echo "========================================"

# 1. Clean generated code
echo "[1/4] Cleaning generated files..."
rm -rf "$GEN_PYTHON_DIR/quantra"
mkdir -p "$GEN_PYTHON_DIR"
mkdir -p "$GEN_CPP_DIR"

# 2. Generate C++ Code (Restored)
echo "[2/4] Generating C++ code..."
flatc --cpp --gen-all -o "$GEN_CPP_DIR" "$FBS_DIR"/*.fbs

# 3. Generate Python Code (Native Object API)
echo "[3/4] Generating Python code..."
flatc --python --gen-object-api --gen-all -o "$GEN_PYTHON_DIR" "$FBS_DIR"/*.fbs
touch "$GEN_PYTHON_DIR/quantra/__init__.py"

# 4. Smart Patching
echo "[4/4] Running Smart Import Fixer..."
# We run the python script we just created
python3 "$TOOLS_DIR/fix_imports.py" "$GEN_PYTHON_DIR/quantra"

echo "Done! Build Complete."