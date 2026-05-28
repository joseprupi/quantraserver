#ifndef QUANTRA_BASIS_SWAP_MAPPER_H
#define QUANTRA_BASIS_SWAP_MAPPER_H

/**
 * BasisSwapMapper — the one place basis-swap flatbuffers live.
 * Decodes a PriceBasisSwapRequest into plain BasisSwapInputs and serializes
 * the BasisSwapResult back into a PriceBasisSwapResponse.
 */

#include "flatbuffers/grpc.h"

#include "basis_swap_pricer.h"
#include "basis_swap_response_generated.h"
#include "price_basis_swap_request_generated.h"

namespace quantra {

class BasisSwapMapper {
public:
    BasisSwapInputs toInputs(const quantra::PriceBasisSwapRequest* req) const;

    flatbuffers::Offset<quantra::PriceBasisSwapResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const BasisSwapResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_BASIS_SWAP_MAPPER_H
