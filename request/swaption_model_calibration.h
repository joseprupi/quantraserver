#ifndef QUANTRA_SWAPTION_MODEL_CALIBRATION_H
#define QUANTRA_SWAPTION_MODEL_CALIBRATION_H

#include "model_domain.h"
#include "pricing_registry.h"
#include "model_generated.h"

namespace quantra {

struct HwCalibResult {
    double a;
    double sigma;
    double rmse;
    int numHelpers;
    int gridRows;
    int gridCols;
    int gridPoints;
};

HwCalibResult calibrateHullWhiteFromSwaptionVol(
    const PricingRegistry& reg,
    const quantra::SwaptionHwCalibrationSpec* calibSpec,
    const QuantLib::Date asOf);

/// FB-free overload accepting the plain-domain mirror of SwaptionHwCalibrationSpec.
/// Used by the swaption pricer cutover, which never sees the raw FB table.
HwCalibResult calibrateHullWhiteFromSwaptionVol(
    const PricingRegistry& reg,
    const SwaptionHwCalibrationDomain& calibSpec,
    const QuantLib::Date asOf);

// Test hook for per-request cache verification.
void resetHwCalibrationCallCount();
int getHwCalibrationCallCount();

} // namespace quantra

#endif // QUANTRA_SWAPTION_MODEL_CALIBRATION_H
