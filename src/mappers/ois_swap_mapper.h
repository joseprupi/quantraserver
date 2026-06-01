#ifndef QUANTRA_OIS_SWAP_MAPPER_H
#define QUANTRA_OIS_SWAP_MAPPER_H

/**
 * OisSwapMapper — the one place OIS swap flatbuffers live.
 * Decodes a PriceOisSwapRequest into plain OisSwapInputs and serializes the
 * OisSwapResult back into a PriceOisSwapResponse.
 */

#include "flatbuffers/grpc.h"

#include "ois_swap_evaluator.h"
#include "ois_swap_response_generated.h"
#include "price_ois_swap_request_generated.h"

namespace quantra {

class OisSwapMapper {
public:
    OisSwapInputs toInputs(const quantra::PriceOisSwapRequest* req) const;

    flatbuffers::Offset<quantra::PriceOisSwapResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const OisSwapResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_OIS_SWAP_MAPPER_H
