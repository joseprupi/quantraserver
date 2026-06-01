#!/bin/bash
# check_evaluator_boundary.sh — enforce the one rule: an evaluator must not see
# FlatBuffers or gRPC. Scope is parser/*_evaluator.{h,cpp} only. Mappers and
# the legacy *_pricing_service.* files are intentionally out of scope.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORKSPACE="$(cd "${SCRIPT_DIR}/.." && pwd)"

# Collect evaluator files. nullglob makes the glob expand to nothing (rather
# than the literal pattern) when there are no matches — required so the
# check is vacuously green on a tree with no evaluators yet.
shopt -s nullglob
files=("${WORKSPACE}"/parser/*_evaluator.h "${WORKSPACE}"/parser/*_evaluator.cpp)
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
    echo "Evaluator boundary violations: $violations" >&2
    exit 1
fi

echo "Evaluator boundary OK (${#files[@]} files checked)"
