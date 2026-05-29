#include "calibrate_swaption_model_pricer.h"

#include "swaption_model_calibration.h"

namespace quantra {

CalibrateSwaptionModelResult CalibrateSwaptionModelPricer::price(
    const CalibrateSwaptionModelInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    // ctx.asOf is the evaluation date the registry build already installed.
    auto result = calibrateHullWhiteFromSwaptionVol(reg, inputs.calibration, ctx.asOf);

    CalibrateSwaptionModelResult out;
    out.modelId = inputs.modelId;
    out.a = result.a;
    out.sigma = result.sigma;
    out.rmse = result.rmse;
    out.numHelpers = result.numHelpers;
    out.gridRows = result.gridRows;
    out.gridCols = result.gridCols;
    out.gridPoints = result.gridPoints;
    return out;
}

} // namespace quantra
