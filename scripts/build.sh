#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(dirname "$SCRIPT_DIR")"
BUILD_TYPE="${1:-Debug}"
DEPS_PREFIX="${DEPS_INSTALL_PREFIX:-/opt/quantra-deps}"

export DEPS_INSTALL_PREFIX="$DEPS_PREFIX"
export PATH="$DEPS_PREFIX/bin:$PATH"

cd "$WORKSPACE"

if ! command -v flatc >/dev/null 2>&1; then
    echo "ERROR: flatc not found. Set DEPS_INSTALL_PREFIX so build.sh can find the FlatBuffers toolchain." >&2
    exit 1
fi

echo "=== Regenerating schemas and generated code ==="
bash "$SCRIPT_DIR/generate_schemas.sh"

echo "=== Cleaning build directory ==="
rm -rf "$WORKSPACE/build"
mkdir -p "$WORKSPACE/build"

cd "$WORKSPACE/build"
cmake -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" ..
make -j"$(nproc)"

echo "Build complete!"
