#!/bin/bash
# check_pricer_boundary.sh — enforce the one rule: a pricer must not see
# FlatBuffers or gRPC. Scope is parser/*_pricer.{h,cpp} only. Mappers and
# the legacy *_pricing_service.* files are intentionally out of scope.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Collect pricer files. nullglob makes the glob expand to nothing (rather
# than the literal pattern) when there are no matches — required so the
# check is vacuously green on a tree with no pricers yet.
shopt -s nullglob
files=("${WORKSPACE}"/parser/*_pricer.h "${WORKSPACE}"/parser/*_pricer.cpp)
shopt -u nullglob

violations=0
for f in "${files[@]}"; do
    # #include "...something_generated.h" or <...something_generated.h>
    if grep -HnE '^[[:space:]]*#include[[:space:]]*[<"][^>"]*_generated\.h[>"]' "$f"; then
        violations=$((violations + 1))
    fi
    if grep -HnE 'flatbuffers::' "$f"; then
        violations=$((violations + 1))
    fi
    if grep -HnE 'grpc::' "$f"; then
        violations=$((violations + 1))
    fi
done

if [ "$violations" -ne 0 ]; then
    echo "Pricer boundary violations: $violations" >&2
    exit 1
fi

echo "Pricer boundary OK (${#files[@]} files checked)"
