#include "calibrate_swaption_vol_mapper.h"

#include "error.h"
#include "swaption_vol_diagnostics.h"

namespace quantra {

CalibrateSwaptionVolInputs CalibrateSwaptionVolMapper::toInputs(
    const quantra::CalibrateSwaptionVolRequest* req) const {
    if (!req || !req->pricing() || !req->vol_id() ||
        !req->discounting_curve_id() || !req->forwarding_curve_id()) {
        QUANTRA_INVALID_ARGUMENT(
            "CalibrateSwaptionVolRequest requires pricing, vol_id, "
            "discounting_curve_id, and forwarding_curve_id");
    }

    CalibrateSwaptionVolInputs inputs;
    inputs.volId = req->vol_id()->str();
    inputs.discountingCurveId = req->discounting_curve_id()->str();
    inputs.forwardingCurveId = req->forwarding_curve_id()->str();
    if (inputs.volId.empty() || inputs.discountingCurveId.empty() ||
        inputs.forwardingCurveId.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            "CalibrateSwaptionVolRequest fields vol_id, discounting_curve_id, "
            "forwarding_curve_id must be non-empty");
    }
    return inputs;
}

flatbuffers::Offset<quantra::CalibrateSwaptionVolResponse>
CalibrateSwaptionVolMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const CalibrateSwaptionVolResult& result) const {
    auto diagnosticsOff = quantra::buildSwaptionVolDiagnostics(
        builder, result.volId, result.finalized, /*extraWarnings=*/{});

    auto volIdOff = builder.CreateString(result.volId);
    quantra::CalibrateSwaptionVolResponseBuilder rb(builder);
    rb.add_vol_id(volIdOff);
    rb.add_diagnostics(diagnosticsOff);
    return rb.Finish();
}

} // namespace quantra
