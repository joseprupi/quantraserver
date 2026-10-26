#ifndef QUANTRA_IMPLIED_FORWARDS_MAPPER_H
#define QUANTRA_IMPLIED_FORWARDS_MAPPER_H

/**
 * ImpliedForwardsMapper — the one place this product's flatbuffers live.
 * Decodes a CalibrateImpliedForwardsRequest into plain ImpliedForwardsInputs
 * (the quote lines with absent scalars kept absent, the filter with schema
 * defaults, the ids) and serializes an ImpliedForwardsResult back into a
 * CalibrateImpliedForwardsResponse, including the ready-to-paste
 * InterpolatedZero dividend TermStructure.
 */

#include "implied_forwards_evaluator.h"

#include "calibrate_implied_forwards_request_generated.h"
#include "calibrate_implied_forwards_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class ImpliedForwardsMapper {
public:
    ImpliedForwardsInputs toInputs(const quantra::CalibrateImpliedForwardsRequest* req) const;

    flatbuffers::Offset<quantra::CalibrateImpliedForwardsResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const ImpliedForwardsResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_IMPLIED_FORWARDS_MAPPER_H
