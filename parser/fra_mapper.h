#ifndef QUANTRA_FRA_MAPPER_H
#define QUANTRA_FRA_MAPPER_H

/**
 * FraMapper — the one place FRA flatbuffers live.
 * Decodes a PriceFRARequest into plain FraInputs and serializes the
 * FraResult back into a PriceFRAResponse.
 */

#include "flatbuffers/grpc.h"

#include "fra_evaluator.h"
#include "fra_response_generated.h"
#include "price_fra_request_generated.h"

namespace quantra {

class FraMapper {
public:
    FraInputs toInputs(const quantra::PriceFRARequest* req) const;

    flatbuffers::Offset<quantra::PriceFRAResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const FraResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_FRA_MAPPER_H
