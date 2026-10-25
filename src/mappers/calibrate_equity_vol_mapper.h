#ifndef QUANTRA_CALIBRATE_EQUITY_VOL_MAPPER_H
#define QUANTRA_CALIBRATE_EQUITY_VOL_MAPPER_H

/**
 * CalibrateEquityVolMapper — the one place this product's flatbuffers live.
 * Decodes a CalibrateEquityVolRequest into plain CalibrateEquityVolInputs (the surface id) and
 * serializes a CalibrateEquityVolResult into a CalibrateEquityVolResponse: the calibrated
 * slices (theta, rho, psi and the fitted forward on every slice, so that pasting them into a
 * BlackVolSpec with shape=SsviParams reproduces the surface) and the EquityVolDiagnostics block.
 */

#include "calibrate_equity_vol_evaluator.h"

#include "calibrate_equity_vol_request_generated.h"
#include "calibrate_equity_vol_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class CalibrateEquityVolMapper {
public:
    CalibrateEquityVolInputs toInputs(const quantra::CalibrateEquityVolRequest* req) const;

    flatbuffers::Offset<quantra::CalibrateEquityVolResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const CalibrateEquityVolResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CALIBRATE_EQUITY_VOL_MAPPER_H
