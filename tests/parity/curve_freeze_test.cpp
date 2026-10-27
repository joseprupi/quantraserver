// The curve cache holds a helper-free copy of each bootstrapped curve
// (CurveSerializer::freeze). For every trait and interpolator the parser
// builds, the copy must return exactly what the bootstrapped curve returns
// (discount factors, zero and forward rates, inside and past the curve), fail
// where it fails, and stay put when the evaluation date moves.
#include "curve_registry.h"
#include "curve_serializer.h"
#include "date_convert.h"
#include "index_registry.h"
#include "quote_registry.h"
#include "term_structure_parser.h"

#include "term_structure_generated.h"

#include <ql/errors.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace quantra { namespace testing { namespace {

const QuantLib::Date kAsOf(15, QuantLib::January, 2025);

constexpr auto kDiscount = quantra::enums::BootstrapTrait_Discount;
constexpr auto kZeroRate = quantra::enums::BootstrapTrait_ZeroRate;
constexpr auto kFwdRate = quantra::enums::BootstrapTrait_FwdRate;
constexpr auto kLinear = quantra::enums::Interpolator_Linear;
constexpr auto kLogLinear = quantra::enums::Interpolator_LogLinear;
constexpr auto kLogCubic = quantra::enums::Interpolator_LogCubic;
constexpr auto kBackwardFlat = quantra::enums::Interpolator_BackwardFlat;
constexpr auto kForwardFlat = quantra::enums::Interpolator_ForwardFlat;

struct FreezeCase {
    const char* name;
    quantra::enums::BootstrapTrait trait;
    quantra::enums::Interpolator interpolator;
    /// QuantLib cannot bootstrap a log-interpolated zero or forward rate (the
    /// bracket reaches -1) nor a forward-flat forward (the last node never
    /// reaches its own deposit).
    bool bootstraps;
};

/// Deposits from the reference date, rates rising then falling, as a curve of
/// the given trait and interpolator built by TermStructureParser.
std::shared_ptr<QuantLib::YieldTermStructure> buildDepositCurve(
    flatbuffers::FlatBufferBuilder& fbb, const FreezeCase& c) {
    const std::vector<std::pair<int, double>> pillars{{1, 0.0290}, {3, 0.0310},  {6, 0.0335},
                                                      {9, 0.0350}, {12, 0.0345}, {18, 0.0330},
                                                      {24, 0.0320}};
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts;
    for (const auto& [months, rate] : pillars) {
        auto tenor = quantra::CreatePeriod(fbb, months, quantra::enums::TimeUnit_Months);
        auto deposit = quantra::CreateDepositHelper(
            fbb, rate, tenor, 0, quantra::enums::Calendar_TARGET,
            quantra::enums::BusinessDayConvention_ModifiedFollowing,
            quantra::enums::DayCounter_Actual360);
        pts.push_back(
            quantra::CreatePointsWrapper(fbb, quantra::Point_DepositHelper, deposit.Union()));
    }
    fbb.Finish(
        quantra::CreateTermStructureDirect(
            fbb, "DEPO", quantra::enums::DayCounter_Actual365Fixed, c.interpolator, c.trait, &pts,
            "2025-01-15"));
    const auto* ts = flatbuffers::GetRoot<quantra::TermStructure>(fbb.GetBufferPointer());
    QuoteRegistry quotes;
    CurveRegistry curves;
    IndexRegistry indices;
    return TermStructureParser().parse(ts, &quotes, &curves, &indices);
}

/// Every day from the reference date to a year past the max date.
std::vector<QuantLib::Date> sampleDates(const QuantLib::YieldTermStructure& curve) {
    std::vector<QuantLib::Date> dates;
    for (QuantLib::Date d = curve.referenceDate(); d <= curve.maxDate() + 365; ++d)
        dates.push_back(d);
    return dates;
}

/// The first date where the two curves differ in discount factor, zero rate
/// or one-day forward rate (extrapolating past the max date); none if equal.
std::string firstDifference(
    const QuantLib::YieldTermStructure& a,
    const QuantLib::YieldTermStructure& b,
    const std::vector<QuantLib::Date>& dates) {
    const QuantLib::DayCounter dc = a.dayCounter();
    for (const auto& d : dates) {
        const bool same =
            a.discount(d, true) == b.discount(d, true) &&
            a.zeroRate(d, dc, QuantLib::Continuous, QuantLib::NoFrequency, true).rate() ==
                b.zeroRate(d, dc, QuantLib::Continuous, QuantLib::NoFrequency, true).rate() &&
            a.forwardRate(d, d + 1, dc, QuantLib::Continuous, QuantLib::NoFrequency, true).rate() ==
                b.forwardRate(d, d + 1, dc, QuantLib::Continuous, QuantLib::NoFrequency, true)
                    .rate();
        if (!same) return DateToIso(d);
    }
    return "";
}

class CurveFreezeTest : public ::testing::TestWithParam<FreezeCase> {};

TEST_P(CurveFreezeTest, CopyReturnsExactlyWhatTheBootstrappedCurveReturns) {
    const FreezeCase& c = GetParam();
    QuantLib::SavedSettings restore;
    QuantLib::Settings::instance().evaluationDate() = kAsOf;
    flatbuffers::FlatBufferBuilder fbb;
    const auto live = buildDepositCurve(fbb, c);

    if (!c.bootstraps) {
        EXPECT_THROW(live->discount(1.0), QuantLib::Error);
        EXPECT_EQ(CurveSerializer::freeze(live), nullptr);
        return;
    }

    const auto frozen = CurveSerializer::freeze(live);
    ASSERT_NE(frozen, nullptr);
    EXPECT_EQ(frozen->referenceDate(), live->referenceDate());
    EXPECT_EQ(frozen->maxDate(), live->maxDate());
    EXPECT_EQ(frozen->dayCounter(), live->dayCounter());
    EXPECT_FALSE(frozen->allowsExtrapolation());

    const auto dates = sampleDates(*live);
    EXPECT_EQ(firstDifference(*live, *frozen, dates), "");

    // Past the max date without extrapolation both refuse.
    EXPECT_THROW(live->discount(live->maxDate() + 1), QuantLib::Error);
    EXPECT_THROW(frozen->discount(live->maxDate() + 1), QuantLib::Error);

    // The copy observes nothing: moving the evaluation date leaves it as is.
    std::vector<double> before;
    for (const auto& d : dates)
        before.push_back(frozen->discount(d, true));
    QuantLib::Settings::instance().evaluationDate() = QuantLib::Date(3, QuantLib::March, 2025);
    std::vector<double> after;
    for (const auto& d : dates)
        after.push_back(frozen->discount(d, true));
    EXPECT_EQ(after, before);
}

INSTANTIATE_TEST_SUITE_P(
    AllTraitsAndInterpolators,
    CurveFreezeTest,
    ::testing::Values(
        FreezeCase{"Discount_Linear", kDiscount, kLinear, true},
        FreezeCase{"Discount_LogLinear", kDiscount, kLogLinear, true},
        FreezeCase{"Discount_LogCubic", kDiscount, kLogCubic, true},
        FreezeCase{"Discount_BackwardFlat", kDiscount, kBackwardFlat, true},
        FreezeCase{"Discount_ForwardFlat", kDiscount, kForwardFlat, true},
        FreezeCase{"ZeroRate_Linear", kZeroRate, kLinear, true},
        FreezeCase{"ZeroRate_LogLinear", kZeroRate, kLogLinear, false},
        FreezeCase{"ZeroRate_LogCubic", kZeroRate, kLogCubic, false},
        FreezeCase{"ZeroRate_BackwardFlat", kZeroRate, kBackwardFlat, true},
        FreezeCase{"ZeroRate_ForwardFlat", kZeroRate, kForwardFlat, true},
        FreezeCase{"FwdRate_Linear", kFwdRate, kLinear, true},
        FreezeCase{"FwdRate_LogLinear", kFwdRate, kLogLinear, false},
        FreezeCase{"FwdRate_LogCubic", kFwdRate, kLogCubic, false},
        FreezeCase{"FwdRate_BackwardFlat", kFwdRate, kBackwardFlat, true},
        FreezeCase{"FwdRate_ForwardFlat", kFwdRate, kForwardFlat, false}),
    [](const ::testing::TestParamInfo<FreezeCase>& info) { return std::string(info.param.name); });

// Only bootstrapped curves are copied; anything else is left to the caller.
TEST(CurveFreeze, CurveNotBootstrappedIsNotCopied) {
    auto flat = std::make_shared<QuantLib::FlatForward>(kAsOf, 0.03, QuantLib::Actual365Fixed());
    EXPECT_EQ(CurveSerializer::freeze(flat), nullptr);
}

}}} // namespace quantra::testing
