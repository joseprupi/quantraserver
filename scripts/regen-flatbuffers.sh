#!/bin/bash
set -e
cd /workspace
echo "Regenerating Flatbuffers code..."
echo "Using flatc version: $(flatc --version)"

# C++ generation
for fbs in flatbuffers/fbs/*.fbs; do
    echo "Processing $fbs (C++)"
    flatc --cpp -o flatbuffers/cpp/ "$fbs"
done

# Python generation with --gen-all and --gen-object-api
echo "Generating Python code with Object API..."
flatc --python --gen-all --gen-object-api -o flatbuffers/python \
    flatbuffers/fbs/price_fixed_rate_bond_request.fbs \
    flatbuffers/fbs/price_floating_rate_bond_request.fbs \
    flatbuffers/fbs/price_vanilla_swap_request.fbs \
    flatbuffers/fbs/price_swaption_request.fbs \
    flatbuffers/fbs/price_cds_request.fbs \
    flatbuffers/fbs/price_cap_floor_request.fbs \
    flatbuffers/fbs/price_fra_request.fbs \
    flatbuffers/fbs/fixed_rate_bond_response.fbs \
    flatbuffers/fbs/floating_rate_bond_response.fbs \
    flatbuffers/fbs/vanilla_swap_response.fbs \
    flatbuffers/fbs/swaption_response.fbs \
    flatbuffers/fbs/cds_response.fbs \
    flatbuffers/fbs/cap_floor_response.fbs \
    flatbuffers/fbs/fra_response.fbs

# gRPC
flatc --grpc --cpp -o grpc/ grpc/quantraserver.fbs

echo "Done!"