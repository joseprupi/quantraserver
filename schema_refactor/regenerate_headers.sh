#!/bin/bash
# Run this in your container where flatc is available

FBS_DIR="${1:-/workspace/flatbuffers/fbs}"
CPP_DIR="${2:-/workspace/flatbuffers/cpp}"
FLATC="${3:-flatc}"

echo "Regenerating C++ headers from FlatBuffers schemas..."
echo "FBS_DIR: $FBS_DIR"
echo "CPP_DIR: $CPP_DIR"

mkdir -p "$CPP_DIR"

for fbs in "$FBS_DIR"/*.fbs; do
    filename=$(basename "$fbs")
    echo "  Processing: $filename"
    $FLATC --cpp --grpc -o "$CPP_DIR" -I "$FBS_DIR" "$fbs"
    if [ $? -ne 0 ]; then
        echo "ERROR: Failed to generate $filename"
        exit 1
    fi
done

echo "Done! Generated $(ls -1 $CPP_DIR/*.h 2>/dev/null | wc -l) header files"
