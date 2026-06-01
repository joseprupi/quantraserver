#ifndef QUANTRA_BOOTSTRAP_CURVES_MAPPER_H
#define QUANTRA_BOOTSTRAP_CURVES_MAPPER_H

/**
 * BootstrapCurvesMapper — the one place this product's flatbuffers live.
 * Decodes a BootstrapCurvesRequest into plain BootstrapCurvesInputs (grid
 * dates and pillar dates pre-resolved, sampling specs fully populated) and
 * serializes the BootstrapCurvesResult back into a BootstrapCurvesResponse.
 */

#include "flatbuffers/grpc.h"

#include "bootstrap_curves_evaluator.h"
#include "bootstrap_curves_request_generated.h"
#include "bootstrap_curves_response_generated.h"

namespace quantra {

class BootstrapCurvesMapper {
public:
    BootstrapCurvesInputs toInputs(
        const quantra::BootstrapCurvesRequest* req) const;

    flatbuffers::Offset<quantra::BootstrapCurvesResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const BootstrapCurvesResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_BOOTSTRAP_CURVES_MAPPER_H
