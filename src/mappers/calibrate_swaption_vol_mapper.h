#ifndef QUANTRA_CALIBRATE_SWAPTION_VOL_MAPPER_H
#define QUANTRA_CALIBRATE_SWAPTION_VOL_MAPPER_H

/**
 * CalibrateSwaptionVolMapper — the one place this product's flatbuffers live.
 * Decodes a CalibrateSwaptionVolRequest into plain CalibrateSwaptionVolInputs
 * (the surface id to calibrate plus the discount/forwarding curve ids) and
 * serializes a CalibrateSwaptionVolResult back into a
 * CalibrateSwaptionVolResponse, building the SwaptionVolDiagnostics sub-table
 * from the finalized entry via the shared diagnostics builder.
 */

#include "flatbuffers/grpc.h"

#include "calibrate_swaption_vol_evaluator.h"
#include "calibrate_swaption_vol_request_generated.h"
#include "calibrate_swaption_vol_response_generated.h"

namespace quantra {

class CalibrateSwaptionVolMapper {
public:
    CalibrateSwaptionVolInputs toInputs(
        const quantra::CalibrateSwaptionVolRequest* req) const;

    flatbuffers::Offset<quantra::CalibrateSwaptionVolResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CalibrateSwaptionVolResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CALIBRATE_SWAPTION_VOL_MAPPER_H
