#ifndef QUANTRA_VOL_SURFACE_VALIDATION_MAPPER_H
#define QUANTRA_VOL_SURFACE_VALIDATION_MAPPER_H

/**
 * VolSurfaceValidationMapper — the one place `/validate-vol-surface`
 * flatbuffers live. Decodes a ValidateVolSurfaceRequest into plain
 * VolSurfaceValidationInputs (the quote sheet and filter through the
 * implied-forwards mapper's rules, the pasted forwards as
 * `/calibrate-vol-surface` decodes them, the Monte Carlo spec as a
 * structured note decodes it) and serializes the result.
 */

#include "vol_surface_validation_evaluator.h"

#include "validate_vol_surface_request_generated.h"
#include "validate_vol_surface_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class VolSurfaceValidationMapper {
public:
    VolSurfaceValidationInputs toInputs(const quantra::ValidateVolSurfaceRequest* req) const;

    flatbuffers::Offset<quantra::ValidateVolSurfaceResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const VolSurfaceValidationResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_VOL_SURFACE_VALIDATION_MAPPER_H
