#ifndef QUANTRA_SWAPTION_MODEL_CALIBRATION_H
#define QUANTRA_SWAPTION_MODEL_CALIBRATION_H

#include "model_domain.h"
#include "pricing_registry.h"

#include <ql/handle.hpp>
#include <ql/math/array.hpp>
#include <ql/math/optimization/constraint.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include <limits>
#include <optional>
#include <string>

namespace quantra {

/// Box constraint on the FIRST model parameter only (Hull-White "a"); sigma is
/// left to the model's own PositiveConstraint, which CalibratedModel::calibrate
/// composes with this one. QuantLib's BoundaryConstraint would bound every
/// parameter with the same interval, which is wrong for sigma. Public so the
/// parity tests can run the same constrained fit directly against QuantLib.
class HwMeanReversionBounds : public QuantLib::Constraint {
private:
    class Impl final : public QuantLib::Constraint::Impl {
    public:
        Impl(double low, double high) : low_(low), high_(high) {}
        bool test(const QuantLib::Array& params) const override {
            return params.empty() || (params[0] >= low_ && params[0] <= high_);
        }
        QuantLib::Array upperBound(const QuantLib::Array& params) const override {
            QuantLib::Array ub(params.size(), std::numeric_limits<double>::max());
            if (!ub.empty()) ub[0] = high_;
            return ub;
        }
        QuantLib::Array lowerBound(const QuantLib::Array& params) const override {
            QuantLib::Array lb(params.size(), -std::numeric_limits<double>::max());
            if (!lb.empty()) lb[0] = low_;
            return lb;
        }

    private:
        double low_, high_;
    };

public:
    HwMeanReversionBounds(double low, double high)
        : QuantLib::Constraint(
              QuantLib::ext::shared_ptr<QuantLib::Constraint::Impl>(
                  new HwMeanReversionBounds::Impl(low, high))) {}
};

struct HwCalibResult {
    double a;
    double sigma;
    double rmse;
    int numHelpers;
    int gridRows;
    int gridCols;
    int gridPoints;
    /// Joint-calibration diagnostic (set iff calibrate_a): relative rmse
    /// increase when "a" is doubled or halved at the calibrated sigma, as
    /// 1 - rmse / max(rmse(2a), rmse(a/2)). 0 means the objective is flat in
    /// "a"; values near 1 mean "a" is sharply identified.
    std::optional<double> aFlatness;
    /// True when aFlatness is below kHwAFlatnessThreshold: the fitted "a" is
    /// not supported by the surface and should not be read as a market value.
    std::optional<bool> aPoorlyIdentified;
};

/// a_flatness below this value flags the calibrated "a" as poorly identified.
constexpr double kHwAFlatnessThreshold = 1.0e-2;

/// QuantLib's FdHullWhiteSwaptionEngine projects the index through a second
/// Hull-White model on the forwarding curve and requires both curves to share
/// day counter and reference date. Empty when they do; otherwise names what
/// differs with both values, e.g. "the same day counter (Actual/360 vs
/// Actual/365 (Fixed))".
std::string fdHullWhiteCurveMismatch(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discount);

/// Calibrates Hull-White to the swaption vol surface named in calibSpec, the
/// plain-domain mirror of SwaptionHwCalibrationSpec.
HwCalibResult calibrateHullWhiteFromSwaptionVol(
    const PricingRegistry& reg,
    const SwaptionHwCalibrationDomain& calibSpec,
    const QuantLib::Date asOf);

// Test hook for per-request cache verification.
void resetHwCalibrationCallCount();
int getHwCalibrationCallCount();

} // namespace quantra

#endif // QUANTRA_SWAPTION_MODEL_CALIBRATION_H
