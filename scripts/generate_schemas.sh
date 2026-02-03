#!/bin/bash
set -e

# =============================================================================
# Quantra Schema Generator
# =============================================================================
# Generates all code and documentation from FlatBuffers schemas:
#   1. C++ headers (for server)
#   2. Python modules (for client)
#   3. JSON schemas (for validation)
#   4. OpenAPI spec + docs (for API documentation)
# =============================================================================

# Define paths
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(dirname "$SCRIPT_DIR")"
FBS_DIR="$WORKSPACE/flatbuffers/fbs"
GEN_PYTHON_DIR="$WORKSPACE/flatbuffers/python"
GEN_CPP_DIR="$WORKSPACE/flatbuffers/cpp"
GEN_JSON_DIR="$WORKSPACE/flatbuffers/json"
OPENAPI_DIR="$WORKSPACE/jsonserver/openapi"
TOOLS_DIR="$WORKSPACE/quantra-python/tools"

echo "========================================"
echo "   Quantra Schema Generator"
echo "========================================"
echo "Workspace: $WORKSPACE"
echo ""

# 1. Clean generated code
echo "[1/6] Cleaning generated files..."
rm -rf "$GEN_PYTHON_DIR/quantra"
rm -f "$GEN_CPP_DIR"/*.h
rm -f "$GEN_JSON_DIR"/*.schema.json
mkdir -p "$GEN_PYTHON_DIR"
mkdir -p "$GEN_CPP_DIR"
mkdir -p "$GEN_JSON_DIR"
mkdir -p "$OPENAPI_DIR"

# 2. Generate C++ Code
echo "[2/6] Generating C++ code..."
flatc --cpp --gen-object-api -I "$FBS_DIR" -o "$GEN_CPP_DIR" "$FBS_DIR"/*.fbs

# 2.5. Generate gRPC Service Code
echo "[3/7] Generating gRPC service code..."
GEN_GRPC_DIR="$WORKSPACE/grpc"
mkdir -p "$GEN_GRPC_DIR"
flatc --grpc --cpp -I "$FBS_DIR" -o "$GEN_GRPC_DIR" "$FBS_DIR/quantraserver.fbs"

# 3. Generate Python Code
echo "[3/6] Generating Python code..."
flatc --python --gen-object-api --gen-all -o "$GEN_PYTHON_DIR" "$FBS_DIR"/*.fbs
touch "$GEN_PYTHON_DIR/quantra/__init__.py"

# 4. Generate JSON Schemas (only for files with root_type)
echo "[4/6] Generating JSON schemas..."
ROOT_FILES=$(grep -lE '^\s*root_type\s+' "$FBS_DIR"/*.fbs || true)
if [ -z "$ROOT_FILES" ]; then
    echo "  No root_type declarations found; skipping."
else
    flatc --jsonschema -I "$FBS_DIR" -o "$GEN_JSON_DIR" $ROOT_FILES
    echo "  Generated $(echo $ROOT_FILES | wc -w) schemas"
fi

# 5. Fix Python imports
echo "[5/6] Fixing Python imports..."
if [ -f "$TOOLS_DIR/fix_imports.py" ]; then
    python3 "$TOOLS_DIR/fix_imports.py" "$GEN_PYTHON_DIR/quantra"
else
    echo "  Warning: fix_imports.py not found, skipping"
fi

# 6. Generate OpenAPI documentation
echo "[6/6] Generating OpenAPI documentation..."
if [ -f "$SCRIPT_DIR/generate_openapi.py" ]; then
    python3 "$SCRIPT_DIR/generate_openapi.py"
else
    echo "  Warning: generate_openapi.py not found, skipping"
fi

echo ""
echo "========================================"
echo "   Build Complete!"
echo "========================================"
echo ""
echo "Generated files:"
echo "  C++:     $GEN_CPP_DIR/*.h"
echo "  Python:  $GEN_PYTHON_DIR/quantra/*.py"
echo "  JSON:    $GEN_JSON_DIR/*.schema.json"
echo "  OpenAPI: $OPENAPI_DIR/openapi3.yaml"
echo ""
echo "To view API docs:"
echo "  python3 -m http.server 9000 -d $OPENAPI_DIR"
echo "  Open http://localhost:9000/docs.html"