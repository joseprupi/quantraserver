#ifndef QUANTRA_CALIBRATE_SWAPTION_MODEL_PRICER_H
#define QUANTRA_CALIBRATE_SWAPTION_MODEL_PRICER_H

/**
 * CalibrateSwaptionModelPricer — pure QuantLib core for the Hull-White swaption
 * model calibration endpoint.
 *
 * The heavy lifting lives in the shared helper
 * calibrateHullWhiteFromSwaptionVol (request/swaption_model_calibration);
 * this pricer just hands it the already-resolved plain calibration domain and
 * shapes the calibrated parameters into a plain result.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * calibrate_swaption_model_mapper.{h,cpp}. The pricer reads market data only
 * through the shared helper (which consumes the plain registry fields
 * reg.rates.curves, reg.rates.swapIndices, reg.volatility.swaptionVols).
 */

#include <string>

#include <ql/time/date.hpp>

#include "model_domain.h"
#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// Plain inputs for one calibration request. The mapper has already resolved
/// the calibration source (request override or the referenced model's
/// hw_calibration block) into a single ready SwaptionHwCalibrationDomain.
struct CalibrateSwaptionModelInputs {
    std::string modelId;
    SwaptionHwCalibrationDomain calibration;
};

/// Calibration outcome. Mirrors the CalibrateSwaptionModelResponse FB schema
/// exactly so the mapper just copies fields.
struct CalibrateSwaptionModelResult {
    std::string modelId;
    double a = 0.0;
    double sigma = 0.0;
    double rmse = 0.0;
    int numHelpers = 0;
    int gridRows = 0;
    int gridCols = 0;
    int gridPoints = 0;
};

class CalibrateSwaptionModelPricer {
public:
    CalibrateSwaptionModelResult price(const CalibrateSwaptionModelInputs& inputs,
                                       const PricingRegistry& reg,
                                       const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CALIBRATE_SWAPTION_MODEL_PRICER_H
