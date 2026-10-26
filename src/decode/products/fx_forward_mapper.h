#ifndef QUANTRA_FX_FORWARD_MAPPER_H
#define QUANTRA_FX_FORWARD_MAPPER_H

/**
 * FxForwardMapper — the one place FX forward flatbuffers live.
 * Decodes a PriceFxForwardRequest into plain FxForwardInputs (validating
 * every field that can be checked without market data) and serializes the
 * FxForwardResult back into a PriceFxForwardResponse.
 */

#include "fx_forward_evaluator.h"

#include "fx_forward_response_generated.h"
#include "price_fx_forward_request_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class FxForwardMapper {
public:
    FxForwardInputs toInputs(const quantra::PriceFxForwardRequest* req) const;

    flatbuffers::Offset<quantra::PriceFxForwardResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const FxForwardResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_FX_FORWARD_MAPPER_H
