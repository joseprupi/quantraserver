#ifndef QUANTRA_VOL_SURFACE_CALIBRATION_MAPPER_H
#define QUANTRA_VOL_SURFACE_CALIBRATION_MAPPER_H

/**
 * VolSurfaceCalibrationMapper — the one place this product's flatbuffers
 * live. Decodes a CalibrateVolSurfaceRequest into plain
 * VolSurfaceCalibrationInputs (the quote sheet through the implied-forwards
 * mapper's rules, the optional pasted forwards, the method, the output grid,
 * the SVI options) and serializes a VolSurfaceCalibrationResult back into a
 * CalibrateVolSurfaceResponse, including the ready-to-paste VolSurfaceSpec
 * (BlackVolSpec, SmileCube3D).
 */

#include "vol_surface_calibration_evaluator.h"

#include "calibrate_vol_surface_request_generated.h"
#include "calibrate_vol_surface_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class VolSurfaceCalibrationMapper {
public:
    VolSurfaceCalibrationInputs toInputs(const quantra::CalibrateVolSurfaceRequest* req) const;

    flatbuffers::Offset<quantra::CalibrateVolSurfaceResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const VolSurfaceCalibrationResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_VOL_SURFACE_CALIBRATION_MAPPER_H
