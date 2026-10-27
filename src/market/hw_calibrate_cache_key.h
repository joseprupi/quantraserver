#ifndef QUANTRA_HW_CALIBRATE_CACHE_KEY_H
#define QUANTRA_HW_CALIBRATE_CACHE_KEY_H

#include <ql/termstructures/volatility/volatilitytype.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/frequency.hpp>
#include <ql/time/period.hpp>

#include <string>
#include <vector>

namespace quantra {

/**
 * All inputs consumed by a Hull-White swaption-model calibration that affect
 * its output. Assembled by the calibration seam after grid resolution / input
 * validation and hashed by buildHwCalibrateCacheKey.
 *
 * Curve identity is delegated to the curve cache: `discountCurveKey` /
 * `forwardingCurveKey` are the "yc:v3:<hex>" keys the bootstrapper emitted for
 * the two curves. They carry the full bootstrap identity, so we don't
 * re-serialize curve internals. If either is empty the caller must NOT build a
 * key (calibrate live, uncached) — that is the fail-closed contract.
 */
struct HwCalibrateKeyInputs {
    // Market vols the calibration actually reads, one per (expiry, tenor) node,
    // row-major over the resolved grid (expiries outer, tenors inner). <=400.
    std::vector<double> consumedVols;
    std::vector<QuantLib::Period> expiries;
    std::vector<QuantLib::Period> tenors;

    QuantLib::VolatilityType volType = QuantLib::ShiftedLognormal;
    double displacement = 0.0;
    QuantLib::Date volReferenceDate;

    std::string discountCurveKey;
    std::string forwardingCurveKey;

    // Swap-index identity fields actually consumed by the helper build.
    std::string swapIndexId;
    std::string floatIndexId;
    QuantLib::Frequency fixedFrequency = QuantLib::Annual;
    QuantLib::DayCounter fixedDayCounter;
    int spotDays = 0;
    QuantLib::Calendar fixedCalendar;
    QuantLib::Calendar floatCalendar;

    // IBOR index identity (cloned onto the forwarding curve) as consumed.
    QuantLib::Period iborTenor;
    QuantLib::DayCounter iborDayCounter;
    QuantLib::BusinessDayConvention iborConvention = QuantLib::ModifiedFollowing;
    bool iborEndOfMonth = false;
    QuantLib::Calendar iborFixingCalendar;
    // QuantLib's SwaptionHelper starts each underlying swap this many business
    // days after expiry; the settlementDays it is given (spotDays above) only
    // selects that rule.
    int iborFixingDays = 0;
    // The helper builds an overnight-indexed swap on an overnight index.
    bool iborIsOvernight = false;

    // Calibration spec. Iteration/eval counts are the CLAMPED values actually
    // handed to the optimizer (a client cannot vary a cache entry by asking for
    // more work than the server ceiling permits).
    bool calibrateA = true;
    bool calibrateSigma = true;
    double aInit = 0.0;
    double sigmaInit = 0.0;
    double aLower = 0.0;
    double aUpper = 0.0;
    int maxIterations = 0;       // clamped
    int functionEvaluations = 0; // clamped
    double endCriteriaEps = 0.0;

    QuantLib::Date asOf;
};

/**
 * Build a content-keyed SHA-256 cache key for a Hull-White swaption-model
 * calibration. Output: "hw-calib:v3:<sha256hex>".
 *
 * Every field of HwCalibrateKeyInputs is serialized into a canonical buffer, so
 * two requests collide in the cache iff every consumed input matches. When in
 * doubt we key MORE: under-keying would serve wrong parameters.
 */
std::string buildHwCalibrateCacheKey(const HwCalibrateKeyInputs& inputs);

} // namespace quantra

#endif // QUANTRA_HW_CALIBRATE_CACHE_KEY_H
