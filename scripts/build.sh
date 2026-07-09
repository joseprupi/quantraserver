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

# Optional ccache. The clean rebuild above throws away build/ every run, but
# ccache keys on preprocessed source + flags, so it still turns incremental
# edits into second-scale recompiles. The cache lives inside the (mounted)
# workspace so it survives `docker run --rm`. Best effort: if ccache is absent
# and cannot be installed, the build simply proceeds without it.
CMAKE_CCACHE_ARGS=()
if ! command -v ccache >/dev/null 2>&1; then
    apt-get update -qq >/dev/null 2>&1 && apt-get install -y -qq ccache >/dev/null 2>&1 || true
fi
if command -v ccache >/dev/null 2>&1; then
    export CCACHE_DIR="$WORKSPACE/.ccache"
    export CCACHE_MAXSIZE="${CCACHE_MAXSIZE:-2G}"
    CMAKE_CCACHE_ARGS=(-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache)
    echo "=== ccache enabled (CCACHE_DIR=$CCACHE_DIR) ==="
fi

cd "$WORKSPACE/build"
cmake ${CMAKE_CCACHE_ARGS[@]+"${CMAKE_CCACHE_ARGS[@]}"} \
    -DCMAKE_PREFIX_PATH="$DEPS_PREFIX" -DCMAKE_BUILD_TYPE="$BUILD_TYPE" ..
make -j"$(nproc)"

echo "Build complete!"
