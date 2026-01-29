#!/bin/bash
# Generate C++ headers from FlatBuffers schemas

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FBS_DIR="${SCRIPT_DIR}/fbs"
CPP_DIR="${SCRIPT_DIR}/cpp"

mkdir -p "$CPP_DIR"

echo "Generating C++ headers from FlatBuffers schemas..."

# Find flatc
FLATC=$(which flatc 2>/dev/null || echo "/opt/quantra-deps/bin/flatc")
if [ ! -x "$FLATC" ]; then
    echo "ERROR: flatc not found"
    exit 1
fi

echo "Using flatc: $FLATC"

# Generate each schema
for fbs in "$FBS_DIR"/*.fbs; do
    filename=$(basename "$fbs")
    echo "  Generating: $filename"
    $FLATC --cpp -o "$CPP_DIR" -I "$FBS_DIR" "$fbs"
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to generate $filename"
        exit 1
    fi
done

echo ""
echo "Generated files:"
ls -la "$CPP_DIR"/*.h 2>/dev/null | wc -l
echo " header files in $CPP_DIR"
