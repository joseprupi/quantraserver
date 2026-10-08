#!/bin/bash
# format.sh — run clang-format (style: repo-root .clang-format) over the
# hand-written C++ sources. Generated code (flatbuffers/cpp, grpc/*_generated.h,
# grpc/*.grpc.fb.*) and the vendored jsonserver/crow_all.h are never touched.
#
# Usage: scripts/format.sh            # format in place
#        scripts/format.sh --check    # dry run, non-zero exit on any diff
#
# Expected tool: clang-format 23.x (e.g. `pip install clang-format==23.1.3`).
# Override the binary with CLANG_FORMAT=/path/to/clang-format.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "${SCRIPT_DIR}/.." && pwd)"
CLANG_FORMAT="${CLANG_FORMAT:-clang-format}"

mode="-i"
if [ "${1:-}" = "--check" ]; then
    mode="--dry-run -Werror"
fi

cd "${WORKSPACE}"
find src server jsonserver client/cpp tests/parity tests/integration examples \
    -type f \( -name '*.h' -o -name '*.cpp' \) \
    ! -name 'crow_all.h' -print0 |
    xargs -0 "${CLANG_FORMAT}" --style=file ${mode}
