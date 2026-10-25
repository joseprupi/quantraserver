#include "calibrate_equity_vol_evaluator.h"

#include "error.h"

namespace quantra {

CalibrateEquityVolResult CalibrateEquityVolEvaluator::evaluate(
    const CalibrateEquityVolInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    (void)ctx; // evaluation date already installed during registry build.

    auto it = reg.volatility.blackVols.find(inputs.volId);
    if (it == reg.volatility.blackVols.end()) {
        QUANTRA_NOT_FOUND("Black vol surface not found: " + inputs.volId);
    }
    const BlackVolEntry& entry = it->second;
    if (entry.shapeFb != quantra::enums::VolSurfaceShape_SsviCalibrate || !entry.ssvi ||
        !entry.ssvi->fit) {
        QUANTRA_INVALID_ARGUMENT(
            "Endpoint /calibrate-equity-vol only operates on SsviCalibrate surfaces; vol_id '" +
            inputs.volId + "' is not a BlackVolSpec with shape=SsviCalibrate");
    }
    CalibrateEquityVolResult out;
    out.volId = inputs.volId;
    out.data = entry.ssvi;
    return out;
}

} // namespace quantra
