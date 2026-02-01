#!/bin/bash
set -e

# Define paths
WORKSPACE="/workspace"
FBS_DIR="$WORKSPACE/flatbuffers/fbs"
GEN_PYTHON_DIR="$WORKSPACE/flatbuffers/python"
GEN_CPP_DIR="$WORKSPACE/flatbuffers/cpp"
GEN_JSON_DIR="$WORKSPACE/flatbuffers/json"
TOOLS_DIR="$WORKSPACE/quantra-python/tools"

echo "========================================"
echo "   Building Quantra Protocol Buffers"
echo "========================================"

# 1. Clean generated code
echo "[1/5] Cleaning generated files..."
rm -rf "$GEN_PYTHON_DIR/quantra"
mkdir -p "$GEN_PYTHON_DIR"
mkdir -p "$GEN_CPP_DIR"
mkdir -p "$GEN_JSON_DIR"

# 2. Generate C++ Code (with Object API for native structs)
echo "[2/5] Generating C++ code..."
flatc --cpp --gen-object-api -o "$GEN_CPP_DIR" "$FBS_DIR"/*.fbs

# 3. Generate Python Code (Native Object API)
echo "[3/5] Generating Python code..."
flatc --python --gen-object-api --gen-all -o "$GEN_PYTHON_DIR" "$FBS_DIR"/*.fbs
touch "$GEN_PYTHON_DIR/quantra/__init__.py"

# 4. Generate JSON Schemas
echo "[4/5] Generating JSON schemas..."
flatc --jsonschema -o "$GEN_JSON_DIR" "$FBS_DIR"/*.fbs

# 5. Smart Patching
echo "[5/5] Running Smart Import Fixer..."
python3 "$TOOLS_DIR/fix_imports.py" "$GEN_PYTHON_DIR/quantra"

echo "Done! Build Complete."
echo ""
echo "Generated files:"
echo "  C++:    $GEN_CPP_DIR/*.h"
echo "  Python: $GEN_PYTHON_DIR/quantra/*.py"
echo "  JSON:   $GEN_JSON_DIR/*.schema.json"