#ifndef QUANTRA_CAP_FLOOR_MAPPER_H
#define QUANTRA_CAP_FLOOR_MAPPER_H

/**
 * CapFloorMapper — the one place CapFloor flatbuffers live. Decodes a
 * PriceCapFloorRequest into plain CapFloorInputs and serializes a
 * CapFloorResult back into a PriceCapFloorResponse.
 */

#include "cap_floor_evaluator.h"

#include "cap_floor_response_generated.h"
#include "price_cap_floor_request_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class CapFloorMapper {
public:
    CapFloorInputs toInputs(const quantra::PriceCapFloorRequest* req) const;

    flatbuffers::Offset<quantra::PriceCapFloorResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const CapFloorResult& result) const;

    /// The response of a `price_only` request: one NPV per trade
    /// (price_only_response.h).
    flatbuffers::Offset<quantra::PriceCapFloorResponse> toPriceOnlyResponse(
        flatbuffers::grpc::MessageBuilder& builder, const CapFloorResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CAP_FLOOR_MAPPER_H
