#ifndef QUANTRA_CAP_FLOOR_MAPPER_H
#define QUANTRA_CAP_FLOOR_MAPPER_H

/**
 * CapFloorMapper — the one place CapFloor flatbuffers live. Decodes a
 * PriceCapFloorRequest into plain CapFloorInputs and serializes a
 * CapFloorResult back into a PriceCapFloorResponse.
 */

#include "flatbuffers/grpc.h"

#include "cap_floor_pricer.h"
#include "cap_floor_response_generated.h"
#include "price_cap_floor_request_generated.h"

namespace quantra {

class CapFloorMapper {
public:
    CapFloorInputs toInputs(const quantra::PriceCapFloorRequest* req) const;

    flatbuffers::Offset<quantra::PriceCapFloorResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CapFloorResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CAP_FLOOR_MAPPER_H
