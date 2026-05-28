#ifndef QUANTRA_SWAPTION_MAPPER_H
#define QUANTRA_SWAPTION_MAPPER_H

/**
 * SwaptionMapper — the one place swaption flatbuffers live. Decodes a
 * PriceSwaptionRequest into plain SwaptionInputs (including the rebump and
 * per-trade swaption-builder closures captured over the FB request) and
 * serializes a SwaptionResult back into a PriceSwaptionResponse.
 */

#include "flatbuffers/grpc.h"

#include "price_swaption_request_generated.h"
#include "swaption_pricer.h"
#include "swaption_response_generated.h"

namespace quantra {

class SwaptionMapper {
public:
    SwaptionInputs toInputs(const quantra::PriceSwaptionRequest* req) const;

    flatbuffers::Offset<quantra::PriceSwaptionResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const SwaptionResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_SWAPTION_MAPPER_H
