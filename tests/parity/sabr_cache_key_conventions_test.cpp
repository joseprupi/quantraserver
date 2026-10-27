// The SABR calibrated-cube cache key covers the surface conventions the
// calibration reads: the calendar, business-day convention and day counter
// build the ATM matrix and the frozen cube (option dates and times), so two
// surfaces differing only in one of them must not share a cached cube.

#include "sabr_calibrate_cache_key.h"

#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedkingdom.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <string>

namespace quantra { namespace testing { namespace {

SwaptionVolEntry baseEntry() {
    SwaptionVolEntry e{};
    e.nExp = 1;
    e.nTen = 1;
    e.nStrikes = 3;
    e.expiries = {QuantLib::Period(1, QuantLib::Years)};
    e.tenors = {QuantLib::Period(5, QuantLib::Years)};
    e.sabrStrikeSpreads = {-0.01, 0.0, 0.01};
    e.sabrMarketVolsFlat = {0.22, 0.20, 0.21};
    e.sabrBetaFixed = true;
    e.sabrBetaValue = 0.5;
    e.sabrVegaWeightedSmileFit = false;
    e.displacement = 0.0;
    e.qlVolType = QuantLib::ShiftedLognormal;
    e.referenceDate = QuantLib::Date(15, QuantLib::January, 2025);
    e.calendar = QuantLib::TARGET();
    e.businessDayConvention = QuantLib::ModifiedFollowing;
    e.dayCounter = QuantLib::Actual365Fixed();
    e.swapIndexId = "EUR_SWAP_6M";
    return e;
}

std::string key(const SwaptionVolEntry& e) {
    return buildSabrCalibrateCacheKey(e, {0.03}, "disc-key", "fwd-key");
}

TEST(SabrCalibrateCacheKeyConventions, SameInputsGiveTheSameKey) {
    EXPECT_EQ(key(baseEntry()), key(baseEntry()));
}

TEST(SabrCalibrateCacheKeyConventions, DayCounterIsPartOfTheKey) {
    SwaptionVolEntry other = baseEntry();
    other.dayCounter = QuantLib::Actual360();
    EXPECT_NE(key(baseEntry()), key(other));
}

TEST(SabrCalibrateCacheKeyConventions, CalendarIsPartOfTheKey) {
    SwaptionVolEntry other = baseEntry();
    other.calendar = QuantLib::UnitedKingdom();
    EXPECT_NE(key(baseEntry()), key(other));
}

TEST(SabrCalibrateCacheKeyConventions, BusinessDayConventionIsPartOfTheKey) {
    SwaptionVolEntry other = baseEntry();
    other.businessDayConvention = QuantLib::Following;
    EXPECT_NE(key(baseEntry()), key(other));
}

}}} // namespace quantra::testing
