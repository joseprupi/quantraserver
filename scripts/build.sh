#!/bin/bash
set -e
cd /workspace
mkdir -p build
cd build
cmake -DCMAKE_PREFIX_PATH=${DEPS_INSTALL_PREFIX} -DCMAKE_BUILD_TYPE=${1:-Debug} ..
make -j$(nproc)
echo "Build complete!"
