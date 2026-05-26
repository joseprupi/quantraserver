#ifndef QUANTRA_VANILLA_SWAP_MAPPER_H
#define QUANTRA_VANILLA_SWAP_MAPPER_H

/**
 * VanillaSwapMapper — the one place vanilla-swap flatbuffers live.
 * Decodes the PriceVanillaSwapRequest into plain VanillaSwapInputs and
 * serializes the VanillaSwapResult back into a PriceVanillaSwapResponse.
 */

#include "flatbuffers/grpc.h"

#include "price_vanilla_swap_request_generated.h"
#include "vanilla_swap_pricer.h"
#include "vanilla_swap_response_generated.h"

namespace quantra {

class VanillaSwapMapper {
public:
    VanillaSwapInputs toInputs(const quantra::PriceVanillaSwapRequest* req) const;

    flatbuffers::Offset<quantra::PriceVanillaSwapResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const VanillaSwapResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_VANILLA_SWAP_MAPPER_H
