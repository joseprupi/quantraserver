// Cache keys under per-request calendar holiday overrides.
//
// Every process-lifetime cache (curve, SABR cube, Hull-White calibration) holds
// results computed under whatever calendars were in force when the entry was
// built. The active override fingerprint is therefore part of each key:
//   (a) no overrides  -> key bytes unchanged (pinned to the values the builders
//       produced before overrides existed), also after apply + reset;
//   (b) overrides on  -> a different key;
//   (c) different overrides -> different keys;
//   (d) the same overrides listed in a different order -> the same key.

#include "calendar_overrides.h"
#include "curve_cache_key.h"
#include "hw_calibrate_cache_key.h"
#include "sabr_calibrate_cache_key.h"

#include <ql/time/calendars/japan.hpp>
#include <ql/time/calendars/target.hpp>

#include <gtest/gtest.h>

#include <functional>
#include <ostream>
#include <string>
#include <vector>

namespace quantra { namespace testing { namespace {

using QuantLib::Date;

// Keys produced for the inputs below before the override fingerprint was added
// to the key material. A request without overrides must keep hitting entries
// keyed this way, so these values must never change as a side effect.
const char* const kCurveKeyNoOverrides =
    "yc:v3:4a06a2ccdd85fd584b1910aee22eac58d698e9f60696a29f961752f9244344a7";
const char* const kSabrKeyNoOverrides =
    "sabr-cube:v1:b3bf8ff6128ac5f0ad9f03570fb0d64e2cdb5446bbbfa3b3310a78f503ff3f66";
const char* const kHwKeyNoOverrides =
    "hw-calib:v1:065489406b70935b98cbb26a4944aed583bca61f41df6a3ebf6942a12dc6b7e3";

std::string curveKey() {
    flatbuffers::FlatBufferBuilder fbb;
    auto pw = quantra::CreatePointsWrapper(fbb, quantra::Point_NONE, 0);
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts{pw};
    auto ts = quantra::CreateTermStructureDirect(
        fbb, "curve-calendar-overrides", quantra::enums::DayCounter_Actual360,
        quantra::enums::Interpolator_LogLinear, quantra::enums::BootstrapTrait_Discount, &pts,
        "2024-01-15");
    fbb.Finish(ts);
    const auto* root = flatbuffers::GetRoot<quantra::TermStructure>(fbb.GetBufferPointer());
    KeyContext ctx;
    return CurveKeyBuilder::compute("2024-01-15", root, ctx, {});
}

std::string sabrKey() {
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
    e.referenceDate = Date(15, QuantLib::January, 2024);
    e.swapIndexId = "EUR_SWAP_5Y";
    return buildSabrCalibrateCacheKey(e, {0.03}, "disc-key", "fwd-key");
}

std::string hwKey() {
    HwCalibrateKeyInputs in;
    in.consumedVols = {0.20, 0.21};
    in.expiries = {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    in.tenors = {QuantLib::Period(5, QuantLib::Years), QuantLib::Period(5, QuantLib::Years)};
    in.volReferenceDate = Date(15, QuantLib::January, 2024);
    in.discountCurveKey = "disc-key";
    in.forwardingCurveKey = "fwd-key";
    in.swapIndexId = "EUR_SWAP_5Y";
    in.floatIndexId = "EUR_6M";
    in.spotDays = 2;
    in.fixedCalendar = QuantLib::TARGET();
    in.floatCalendar = QuantLib::TARGET();
    in.iborTenor = QuantLib::Period(6, QuantLib::Months);
    in.iborFixingCalendar = QuantLib::TARGET();
    in.aInit = 0.03;
    in.sigmaInit = 0.01;
    in.maxIterations = 400;
    in.functionEvaluations = 1000;
    in.endCriteriaEps = 1e-8;
    in.asOf = Date(15, QuantLib::January, 2024);
    return buildHwCalibrateCacheKey(in);
}

HolidayOverride make(
    const QuantLib::Calendar& calendar, std::vector<Date> added, std::vector<Date> removed) {
    HolidayOverride o;
    o.calendar = calendar;
    o.added = std::move(added);
    o.removed = std::move(removed);
    return o;
}

// Plain business days / a TARGET holiday (Labour Day) used as override dates.
const Date kMar5(5, QuantLib::March, 2024);
const Date kJun13(13, QuantLib::June, 2024);
const Date kJun14(14, QuantLib::June, 2024);
const Date kLabourDay(1, QuantLib::May, 2024);

struct KeyCase {
    const char* name;
    const char* prefix;
    const char* pinnedNoOverrides;
    std::function<std::string()> build;
};

// Readable gtest parameter output (the default dumps the struct's raw bytes).
void PrintTo(const KeyCase& c, std::ostream* os) {
    *os << c.name;
}

class CacheKeyCalendarOverridesTest : public ::testing::TestWithParam<KeyCase> {
protected:
    // Resets on entry and on exit, so no override outlives a test.
    CalendarOverridesGuard guard_;
};

TEST_P(CacheKeyCalendarOverridesTest, NoOverridesKeepsThePreExistingKey) {
    const auto& c = GetParam();

    const std::string before = c.build();
    EXPECT_EQ(before, c.pinnedNoOverrides);

    // An explicitly empty override set, and an entry with no dates, are both
    // "no overrides": they contribute nothing to the key.
    applyCalendarOverrides({});
    EXPECT_EQ(c.build(), before);
    applyCalendarOverrides({make(QuantLib::TARGET(), {}, {})});
    EXPECT_EQ(c.build(), before);

    // Apply + reset returns to exactly the same key.
    applyCalendarOverrides({make(QuantLib::TARGET(), {kJun14}, {kLabourDay})});
    ASSERT_NE(activeCalendarOverridesFingerprint(), "");
    resetCalendarOverrides();
    EXPECT_EQ(c.build(), before);
    EXPECT_EQ(c.build(), c.pinnedNoOverrides);
}

TEST_P(CacheKeyCalendarOverridesTest, ActiveOverridesChangeTheKey) {
    const auto& c = GetParam();
    const std::string plain = c.build();

    applyCalendarOverrides({make(QuantLib::TARGET(), {kJun14}, {})});
    const std::string overridden = c.build();
    EXPECT_NE(overridden, plain);
    EXPECT_EQ(overridden.rfind(c.prefix, 0), 0u);
    // Deterministic while the same overrides stay active.
    EXPECT_EQ(c.build(), overridden);
}

TEST_P(CacheKeyCalendarOverridesTest, DifferentOverridesGiveDifferentKeys) {
    const auto& c = GetParam();
    const std::string plain = c.build();

    std::vector<std::string> keys;
    // A different date, a different calendar, add-vs-remove of one date, and a
    // superset must each be distinguishable from one another.
    applyCalendarOverrides({make(QuantLib::TARGET(), {kJun14}, {})});
    keys.push_back(c.build());
    applyCalendarOverrides({make(QuantLib::TARGET(), {kJun13}, {})});
    keys.push_back(c.build());
    applyCalendarOverrides({make(QuantLib::Japan(), {kJun14}, {})});
    keys.push_back(c.build());
    applyCalendarOverrides({make(QuantLib::TARGET(), {}, {kLabourDay})});
    keys.push_back(c.build());
    applyCalendarOverrides({make(QuantLib::TARGET(), {kLabourDay}, {})});
    keys.push_back(c.build());
    applyCalendarOverrides({make(QuantLib::TARGET(), {kJun14, kJun13}, {})});
    keys.push_back(c.build());
    applyCalendarOverrides(
        {make(QuantLib::TARGET(), {kJun14}, {}), make(QuantLib::Japan(), {kJun14}, {})});
    keys.push_back(c.build());

    for (size_t i = 0; i < keys.size(); ++i) {
        EXPECT_NE(keys[i], plain) << "override set " << i;
        for (size_t j = i + 1; j < keys.size(); ++j) {
            EXPECT_NE(keys[i], keys[j]) << "override sets " << i << " and " << j;
        }
    }
}

TEST_P(CacheKeyCalendarOverridesTest, OverrideOrderDoesNotChangeTheKey) {
    const auto& c = GetParam();

    applyCalendarOverrides({
        make(QuantLib::TARGET(), {kJun14, kMar5}, {kLabourDay}),
        make(QuantLib::Japan(), {kJun14}, {}),
    });
    const std::string first = c.build();

    applyCalendarOverrides({
        make(QuantLib::Japan(), {kJun14}, {}),
        make(QuantLib::TARGET(), {kMar5, kJun14}, {kLabourDay}),
    });
    EXPECT_EQ(c.build(), first);
}

INSTANTIATE_TEST_SUITE_P(
    AllProcessLifetimeCaches,
    CacheKeyCalendarOverridesTest,
    ::testing::Values(
        KeyCase{"Curve", "yc:v3:", kCurveKeyNoOverrides, curveKey},
        KeyCase{"SabrCube", "sabr-cube:v1:", kSabrKeyNoOverrides, sabrKey},
        KeyCase{"HullWhiteCalibration", "hw-calib:v1:", kHwKeyNoOverrides, hwKey}),
    [](const ::testing::TestParamInfo<KeyCase>& info) { return std::string(info.param.name); });

}}} // namespace quantra::testing
