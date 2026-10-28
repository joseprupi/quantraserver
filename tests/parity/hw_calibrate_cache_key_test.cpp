// The Hull-White calibration cache key covers the IBOR index fields the
// calibration helpers read. QuantLib's SwaptionHelper starts each underlying
// swap the index's fixing days after expiry (the settlementDays it is given
// only selects that rule) and builds an overnight-indexed swap on an overnight
// index, so two calibrations differing only in one of them must not share a
// cached result.

#include "hw_calibrate_cache_key.h"

#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>

#include <gtest/gtest.h>

#include <string>

namespace quantra { namespace testing { namespace {

HwCalibrateKeyInputs baseInputs() {
    HwCalibrateKeyInputs in;
    in.consumedVols = {0.20, 0.22, 0.24, 0.25};
    in.expiries = {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    in.tenors = {QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years)};
    in.volReferenceDate = QuantLib::Date(15, QuantLib::January, 2025);
    in.discountCurveKey = "disc-key";
    in.forwardingCurveKey = "fwd-key";
    in.swapIndexId = "EUR_SWAP_6M";
    in.floatIndexId = "EUR_6M";
    in.fixedFrequency = QuantLib::Annual;
    in.fixedDayCounter = QuantLib::Thirty360(QuantLib::Thirty360::BondBasis);
    in.spotDays = 2;
    in.fixedCalendar = QuantLib::TARGET();
    in.floatCalendar = QuantLib::TARGET();
    in.iborTenor = QuantLib::Period(6, QuantLib::Months);
    in.iborDayCounter = QuantLib::Actual360();
    in.iborConvention = QuantLib::ModifiedFollowing;
    in.iborEndOfMonth = false;
    in.iborFixingCalendar = QuantLib::TARGET();
    in.iborFixingDays = 2;
    in.iborIsOvernight = false;
    in.aInit = 0.03;
    in.sigmaInit = 0.01;
    in.aLower = 0.001;
    in.aUpper = 1.0;
    in.maxIterations = 200;
    in.functionEvaluations = 1000;
    in.endCriteriaEps = 1e-8;
    in.asOf = QuantLib::Date(15, QuantLib::January, 2025);
    return in;
}

std::string key(const HwCalibrateKeyInputs& in) {
    return buildHwCalibrateCacheKey(in);
}

TEST(HwCalibrateCacheKey, SameInputsGiveTheSameKey) {
    EXPECT_EQ(key(baseInputs()), key(baseInputs()));
}

// Same swap-index spot days, different index fixing days: the helper dates
// move, so the keys must differ.
TEST(HwCalibrateCacheKey, IborFixingDaysArePartOfTheKey) {
    HwCalibrateKeyInputs other = baseInputs();
    other.iborFixingDays = 0;
    EXPECT_NE(key(baseInputs()), key(other));
}

TEST(HwCalibrateCacheKey, OvernightIndexIsPartOfTheKey) {
    HwCalibrateKeyInputs other = baseInputs();
    other.iborIsOvernight = true;
    EXPECT_NE(key(baseInputs()), key(other));
}

// Helpers projecting on a forwarding curve other than the discount curve are
// priced on the FD engine. Two curve ids can share one content key, so the
// engine choice must be part of the key.
TEST(HwCalibrateCacheKey, FdHelperEngineIsPartOfTheKey) {
    HwCalibrateKeyInputs other = baseInputs();
    other.fdHelperEngine = true;
    EXPECT_NE(key(baseInputs()), key(other));
}

}}} // namespace quantra::testing
