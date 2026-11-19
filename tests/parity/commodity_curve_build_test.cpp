// Commodity curve build (src/commodities/commodity_curve_build): a futures
// contract whose live months are only partly priced, completed from the
// quoted live months.
//
//  a. identity: every live month priced (with or without an interpolation);
//  b. Linear, BackwardFlat and ForwardFlat fills against hand-computed values
//     on the last-trading-date axis in calendar days;
//  c. extrapolation: refused before the first and after the last quoted live
//     month, then allowed with extrapolateFlat (nearest quote);
//  d. expired months: returned as sent, never a point of the line;
//  e. a month whose last trading date is the valuation date is live;
//  f. errors: interpolation missing, no quoted live month, the contract's
//     own rules, the message prefix;
//  g. the six methods on the swap reference strip with five months removed,
//     against independent reference values (flat extrapolation included);
//  h. curved methods: natural spline on three quotes by hand, cubics on two
//     quotes are the straight line, a straight line is reproduced, LogLinear
//     refuses a non-positive quote;
//  i. average-price quotes: the equation space, a unique square system,
//     redundant and conflicting quotes, underdetermined without a rule, Flat
//     and Smooth and their failures, quote validation, a partly realised
//     quote, a quote with no live month, sensitivities against bumps, the
//     fill without quotes, independent reference values on a monthly strip.
#include "commodity_curve_build.h"

#include "commodity_curve_build_evaluator.h"
#include "commodity_futures_contract.h"
#include "commodity_linear_system.h"
#include "commodity_periods.h"
#include "commodity_pricing_days.h"
#include "commodity_spot_price.h"
#include "commodity_swap.h"
#include "error.h"

#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/calendars/weekendsonly.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace quantra::commodities;
using QuantLib::Date;

namespace {

Date D(int y, int m, int d) {
    return Date(d, static_cast<QuantLib::Month>(m), y);
}

const Date kAsOf = D(2026, 11, 13);

void expectInvalid(const std::function<void()>& fn, const std::string& fragment) {
    try {
        fn();
        ADD_FAILURE() << "expected an invalid argument containing '" << fragment << "'";
    } catch (const QuantraInvalidArgument& e) {
        EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos)
            << "message '" << e.what() << "' lacks '" << fragment << "'";
    }
}

ContractMonth M(const std::string& month, Date lastTrade, std::optional<double> price) {
    return ContractMonth{month, lastTrade, price};
}

FuturesContract contract(std::vector<ContractMonth> months) {
    return FuturesContract(
        "SYN_CL", PricingCalendar(QuantLib::WeekendsOnly(), {}), std::move(months), {});
}

/// Five live months (last trading dates 20 Nov 2026 to 22 Mar 2027) and one
/// expired month; `prices` gives each live month's price or nothing.
FuturesContract strip(const std::vector<std::optional<double>>& prices) {
    return contract(
        {M("2026-11", D(2026, 10, 20), std::nullopt), M("2026-12", D(2026, 11, 20), prices[0]),
         M("2027-01", D(2026, 12, 21), prices[1]), M("2027-02", D(2027, 1, 20), prices[2]),
         M("2027-03", D(2027, 2, 22), prices[3]), M("2027-04", D(2027, 3, 22), prices[4])});
}

} // namespace

// ---------------------------------------------------------------------------
// a. identity
// ---------------------------------------------------------------------------

TEST(CommodityCurveBuild, FullyPricedIsTheIdentity) {
    const auto c = strip({59.5, 60.0, 61.0, 62.0, 62.5});
    for (const auto interpolation :
         {std::optional<CurveInterpolation>{},
          std::optional<CurveInterpolation>{CurveInterpolation::Linear}}) {
        const auto r = BuildFuturesCurve(c, interpolation, false, kAsOf);
        ASSERT_EQ(r.months.size(), c.months().size());
        EXPECT_TRUE(r.interpolatedMonths.empty());
        for (std::size_t i = 0; i < r.months.size(); ++i) {
            EXPECT_EQ(r.months[i].month, c.months()[i].month);
            EXPECT_EQ(r.months[i].lastTradeDate, c.months()[i].lastTradeDate);
            EXPECT_EQ(r.months[i].price, c.months()[i].price);
        }
    }
}

// ---------------------------------------------------------------------------
// b. the three interpolations
// ---------------------------------------------------------------------------

TEST(CommodityCurveBuild, LinearInCalendarDaysBetweenLastTradeDates) {
    // 2027-02 (last traded 20 Jan) between 2027-01 (21 Dec, 60.00) and
    // 2027-03 (22 Feb, 62.00): 30 of 63 days -> 60 + 2 * 30 / 63.
    const auto r = BuildFuturesCurve(
        strip({59.5, 60.0, std::nullopt, 62.0, 62.5}), CurveInterpolation::Linear, false, kAsOf);
    ASSERT_TRUE(r.months[3].price);
    EXPECT_NEAR(*r.months[3].price, 60.0 + 2.0 * 30.0 / 63.0, 1e-13);
    EXPECT_EQ(r.interpolatedMonths, std::vector<std::string>{"2027-02"});
    // Quoted months unchanged, the expired month still without a price.
    EXPECT_EQ(*r.months[2].price, 60.0);
    EXPECT_EQ(*r.months[4].price, 62.0);
    EXPECT_FALSE(r.months[0].price);
}

TEST(CommodityCurveBuild, LinearFillsSeveralMonthsBetweenTwoQuotes) {
    // 2027-01 and 2027-02 between 2026-12 (20 Nov, 59.00) and 2027-03 (22
    // Feb, 63.00): 94 days in all; 31 and 61 days in.
    const auto r = BuildFuturesCurve(
        strip({59.0, std::nullopt, std::nullopt, 63.0, std::nullopt}), CurveInterpolation::Linear,
        true, kAsOf);
    EXPECT_NEAR(*r.months[2].price, 59.0 + 4.0 * 31.0 / 94.0, 1e-13);
    EXPECT_NEAR(*r.months[3].price, 59.0 + 4.0 * 61.0 / 94.0, 1e-13);
    EXPECT_EQ(*r.months[5].price, 63.0); // flat after the last quote
    EXPECT_EQ(r.interpolatedMonths, (std::vector<std::string>{"2027-01", "2027-02", "2027-04"}));
}

TEST(CommodityCurveBuild, BackwardFlatReadsTheNextQuote) {
    const auto r = BuildFuturesCurve(
        strip({59.5, 60.0, std::nullopt, 62.0, 62.5}), CurveInterpolation::BackwardFlat, false,
        kAsOf);
    EXPECT_EQ(*r.months[3].price, 62.0);
}

TEST(CommodityCurveBuild, ForwardFlatReadsThePreviousQuote) {
    const auto r = BuildFuturesCurve(
        strip({59.5, 60.0, std::nullopt, 62.0, 62.5}), CurveInterpolation::ForwardFlat, false,
        kAsOf);
    EXPECT_EQ(*r.months[3].price, 60.0);
}

// ---------------------------------------------------------------------------
// c. extrapolation
// ---------------------------------------------------------------------------

TEST(CommodityCurveBuild, AfterTheLastQuoteRefusedThenFlat) {
    const auto c = strip({59.5, 60.0, 61.0, 62.0, std::nullopt});
    expectInvalid(
        [&] { BuildFuturesCurve(c, CurveInterpolation::Linear, false, kAsOf); },
        "contract.months[5] (2027-04): last_trade_date 2027-03-22 is after the last quoted live "
        "month (2026-11-20 to 2027-02-22) and extrapolate_flat is false");
    const auto r = BuildFuturesCurve(c, CurveInterpolation::Linear, true, kAsOf);
    EXPECT_EQ(*r.months[5].price, 62.0);
    EXPECT_EQ(r.interpolatedMonths, std::vector<std::string>{"2027-04"});
}

TEST(CommodityCurveBuild, BeforeTheFirstQuoteRefusedThenFlat) {
    const auto c = strip({std::nullopt, 60.0, 61.0, 62.0, 62.5});
    expectInvalid(
        [&] { BuildFuturesCurve(c, CurveInterpolation::BackwardFlat, false, kAsOf); },
        "contract.months[1] (2026-12): last_trade_date 2026-11-20 is before the first quoted "
        "live month");
    const auto r = BuildFuturesCurve(c, CurveInterpolation::ForwardFlat, true, kAsOf);
    EXPECT_EQ(*r.months[1].price, 60.0);
}

TEST(CommodityCurveBuild, OneQuoteFillsEverythingFlat) {
    const auto r = BuildFuturesCurve(
        strip({std::nullopt, std::nullopt, 61.0, std::nullopt, std::nullopt}),
        CurveInterpolation::Linear, true, kAsOf);
    for (std::size_t i = 1; i < r.months.size(); ++i) {
        EXPECT_EQ(*r.months[i].price, 61.0) << r.months[i].month;
    }
    EXPECT_EQ(r.interpolatedMonths.size(), 4u);
}

// ---------------------------------------------------------------------------
// d. expired months
// ---------------------------------------------------------------------------

TEST(CommodityCurveBuild, ExpiredMonthsAreReturnedAsSentAndNeverRead) {
    // An expired month with a price far off the strip: not a point of the
    // line (the fill uses only live quotes), returned unchanged.
    const auto c = contract(
        {M("2026-10", D(2026, 9, 21), 999.0), M("2026-11", D(2026, 10, 20), std::nullopt),
         M("2026-12", D(2026, 11, 20), std::nullopt), M("2027-01", D(2026, 12, 21), 60.0)});
    const auto r = BuildFuturesCurve(c, CurveInterpolation::Linear, true, kAsOf);
    EXPECT_EQ(*r.months[0].price, 999.0);
    EXPECT_FALSE(r.months[1].price);
    EXPECT_EQ(*r.months[2].price, 60.0);
    EXPECT_EQ(r.interpolatedMonths, std::vector<std::string>{"2026-12"});
}

// ---------------------------------------------------------------------------
// e. the valuation date itself
// ---------------------------------------------------------------------------

TEST(CommodityCurveBuild, MonthExpiringOnTheValuationDateIsLive) {
    // Last traded on the valuation date: a swap can still read today's price
    // on that day, so the month is filled (flat from the next quote here).
    const auto c = contract(
        {M("2026-12", kAsOf, std::nullopt), M("2027-01", D(2026, 12, 21), 60.0),
         M("2027-02", D(2027, 1, 20), 61.0)});
    expectInvalid(
        [&] { BuildFuturesCurve(c, CurveInterpolation::Linear, false, kAsOf); },
        "contract.months[0] (2026-12)");
    const auto r = BuildFuturesCurve(c, CurveInterpolation::Linear, true, kAsOf);
    EXPECT_EQ(*r.months[0].price, 60.0);
    // One day later it has expired: returned without a price.
    const auto later = BuildFuturesCurve(c, CurveInterpolation::Linear, false, kAsOf + 1);
    EXPECT_FALSE(later.months[0].price);
    EXPECT_TRUE(later.interpolatedMonths.empty());
}

// ---------------------------------------------------------------------------
// f. errors
// ---------------------------------------------------------------------------

TEST(CommodityCurveBuild, Errors) {
    expectInvalid(
        [&] {
            BuildFuturesCurve(
                strip({59.5, std::nullopt, 61.0, std::nullopt, 62.5}), std::nullopt, false, kAsOf);
        },
        "interpolation is required: live months without a price (2027-01, 2027-03)");
    expectInvalid(
        [&] {
            BuildFuturesCurve(
                strip({std::nullopt, std::nullopt, std::nullopt, std::nullopt, std::nullopt}),
                CurveInterpolation::Linear, true, kAsOf);
        },
        "contract.months: no live month has a price (last_trade_date on or after 2026-11-13)");
    // The contract's own rules stay its constructor's.
    expectInvalid(
        [&] {
            contract({M("2027-01", D(2026, 12, 21), 60.0), M("2027-02", D(2026, 12, 21), 61.0)});
        },
        "commodity contract 'SYN_CL' months[1]: last_trade_date 2026-12-21 of 2027-02 is not "
        "after 2026-12-21 of 2027-01");
}

TEST(CommodityCurveBuild, MessagePrefix) {
    EXPECT_EQ(
        QualifyCurveBuildMessage("curves[1]", "interpolation is required"),
        "curves[1].interpolation is required");
    EXPECT_EQ(
        QualifyCurveBuildMessage("curves[0]", "contract.months[2] (2027-02): x"),
        "curves[0].contract.months[2] (2027-02): x");
    EXPECT_EQ(QualifyCurveBuildMessage("curves[0]", "other"), "curves[0]: other");
}

// ---------------------------------------------------------------------------
// g. the six methods on one strip: independent reference values
// ---------------------------------------------------------------------------

namespace {

/// The futures strip of the swap reference case (synthetic data, as of
/// 2026-11-13): months 2026-12 to 2028-01 with their last trading dates; the
/// months in `removed` are sent without a price.
FuturesContract referenceStrip(const std::vector<std::string>& removed) {
    const std::vector<std::pair<std::string, Date>> ltd = {
        {"2026-12", D(2026, 11, 20)}, {"2027-01", D(2026, 12, 21)}, {"2027-02", D(2027, 1, 20)},
        {"2027-03", D(2027, 2, 22)},  {"2027-04", D(2027, 3, 22)},  {"2027-05", D(2027, 4, 20)},
        {"2027-06", D(2027, 5, 20)},  {"2027-07", D(2027, 6, 22)},  {"2027-08", D(2027, 7, 20)},
        {"2027-09", D(2027, 8, 20)},  {"2027-10", D(2027, 9, 21)},  {"2027-11", D(2027, 10, 20)},
        {"2027-12", D(2027, 11, 19)}, {"2028-01", D(2027, 12, 20)}};
    const std::vector<double> prices = {60.0, 60.4, 60.7, 61.0, 61.2, 61.4, 61.5,
                                        61.6, 61.7, 61.8, 61.9, 62.0, 62.1, 62.2};
    std::vector<ContractMonth> months;
    for (std::size_t i = 0; i < ltd.size(); ++i) {
        bool drop = false;
        for (const auto& r : removed) {
            drop = drop || r == ltd[i].first;
        }
        months.push_back(
            M(ltd[i].first, ltd[i].second, drop ? std::nullopt : std::optional<double>(prices[i])));
    }
    return contract(months);
}

const std::vector<std::string> kRemoved = {"2027-02", "2027-03", "2027-06", "2027-09", "2027-10"};

std::vector<double> filledPrices(const FuturesCurveBuildResult& r) {
    std::vector<double> out;
    for (const auto& name : r.interpolatedMonths) {
        for (const auto& m : r.months) {
            if (m.month == name) {
                out.push_back(*m.price);
            }
        }
    }
    return out;
}

void expectFilled(
    const std::vector<std::string>& removed,
    CurveInterpolation method,
    bool extrapolateFlat,
    const std::vector<double>& expected) {
    const auto r = BuildFuturesCurve(referenceStrip(removed), method, extrapolateFlat, kAsOf);
    EXPECT_EQ(r.interpolatedMonths, removed);
    const auto got = filledPrices(r);
    ASSERT_EQ(got.size(), expected.size());
    for (std::size_t i = 0; i < got.size(); ++i) {
        EXPECT_NEAR(got[i], expected[i], 1e-9)
            << CurveInterpolationName(method) << " " << removed[i];
    }
}

} // namespace

TEST(CommodityCurveBuildReference, Linear) {
    expectFilled(
        kRemoved, CurveInterpolation::Linear, false,
        {60.6637362637, 60.9538461538, 61.4952380952, 61.8010869565, 61.9054347826});
}

TEST(CommodityCurveBuildReference, LogLinear) {
    expectFilled(
        kRemoved, CurveInterpolation::LogLinear, false,
        {60.6625740259, 60.9527240494, 61.4951569808, 61.8009244485, 61.9052776863});
}

TEST(CommodityCurveBuildReference, BackwardFlat) {
    expectFilled(kRemoved, CurveInterpolation::BackwardFlat, false, {61.2, 61.2, 61.6, 62.0, 62.0});
}

TEST(CommodityCurveBuildReference, ForwardFlat) {
    expectFilled(kRemoved, CurveInterpolation::ForwardFlat, false, {60.4, 60.4, 61.4, 61.7, 61.7});
}

TEST(CommodityCurveBuildReference, KrugerCubic) {
    expectFilled(
        kRemoved, CurveInterpolation::KrugerCubic, false,
        {60.6932374651, 60.9780335193, 61.5034978519, 61.8028587986, 61.9058753205});
}

TEST(CommodityCurveBuildReference, LinearWithFlatExtrapolation) {
    auto removed = kRemoved;
    removed.push_back("2027-12");
    removed.push_back("2028-01");
    expectFilled(
        removed, CurveInterpolation::Linear, true,
        {60.6637362637, 60.9538461538, 61.4952380952, 61.8010869565, 61.9054347826, 62.0, 62.0});
}

// ---------------------------------------------------------------------------
// h. properties of the curved methods
// ---------------------------------------------------------------------------

TEST(CommodityCurveBuild, NaturalCubicOnThreeQuotes) {
    // Quotes 2026-12 (20 Nov, 60.0), 2027-03 (22 Feb, 61.0), 2027-04 (22
    // Mar, 61.2); 2027-01 (21 Dec) and 2027-02 (20 Jan) are filled. Natural
    // spline in days: second derivative 0 at both ends and
    // M1 = 3 (S1 - S0) / (h0 + h1) at the middle quote.
    const auto c = strip({60.0, std::nullopt, std::nullopt, 61.0, 61.2});
    const auto r = BuildFuturesCurve(c, CurveInterpolation::NaturalCubic, false, kAsOf);
    const double h0 = 94.0, h1 = 28.0;
    const double s0 = 1.0 / h0, s1 = 0.2 / h1;
    const double m1 = 3.0 * (s1 - s0) / (h0 + h1);
    auto spline = [&](double t) { // t days after 20 Nov, inside [0, h0]
        return m1 * t * t * t / (6.0 * h0) + 60.0 * (h0 - t) / h0 +
               (61.0 - m1 * h0 * h0 / 6.0) * t / h0;
    };
    EXPECT_NEAR(*r.months[2].price, spline(31.0), 1e-12);
    EXPECT_NEAR(*r.months[3].price, spline(61.0), 1e-12);
}

TEST(CommodityCurveBuild, CubicsOnTwoQuotesAreTheStraightLine) {
    const auto c = strip({std::nullopt, 60.0, std::nullopt, 62.0, std::nullopt});
    for (const auto method :
         {CurveInterpolation::KrugerCubic, CurveInterpolation::NaturalCubic,
          CurveInterpolation::Linear}) {
        const auto r = BuildFuturesCurve(c, method, true, kAsOf);
        EXPECT_NEAR(*r.months[3].price, 60.0 + 2.0 * 30.0 / 63.0, 1e-12)
            << CurveInterpolationName(method);
        // Outside the quotes: the nearest quote, never the cubic continued.
        EXPECT_EQ(*r.months[1].price, 60.0);
        EXPECT_EQ(*r.months[5].price, 62.0);
    }
}

TEST(CommodityCurveBuild, CurvedMethodsReproduceAStraightLine) {
    // Quotes on a straight line in days (0, 61, 122 days after 20 Nov): the
    // straight and cubic methods return the line.
    const auto c =
        strip({60.0, std::nullopt, 60.0 + 0.01 * 61.0, std::nullopt, 60.0 + 0.01 * 122.0});
    for (const auto method :
         {CurveInterpolation::Linear, CurveInterpolation::KrugerCubic,
          CurveInterpolation::NaturalCubic}) {
        const auto r = BuildFuturesCurve(c, method, false, kAsOf);
        EXPECT_NEAR(*r.months[2].price, 60.0 + 0.01 * 31.0, 1e-12)
            << CurveInterpolationName(method);
        EXPECT_NEAR(*r.months[4].price, 60.0 + 0.01 * 94.0, 1e-12)
            << CurveInterpolationName(method);
    }
}

TEST(CommodityCurveBuild, LogLinearNeedsPositiveQuotes) {
    const auto c = strip({-1.0, 60.0, std::nullopt, 62.0, 62.5});
    expectInvalid(
        [&] { BuildFuturesCurve(c, CurveInterpolation::LogLinear, false, kAsOf); },
        "contract.months[1] (2026-12): price -1 must be > 0 for interpolation LogLinear");
    // The same quotes are fine for the other methods.
    EXPECT_NO_THROW(BuildFuturesCurve(c, CurveInterpolation::KrugerCubic, false, kAsOf));
}

TEST(CommodityCurveBuild, MethodNames) {
    EXPECT_EQ(CurveInterpolationName(CurveInterpolation::Linear), "Linear");
    EXPECT_EQ(CurveInterpolationName(CurveInterpolation::LogLinear), "LogLinear");
    EXPECT_EQ(CurveInterpolationName(CurveInterpolation::BackwardFlat), "BackwardFlat");
    EXPECT_EQ(CurveInterpolationName(CurveInterpolation::ForwardFlat), "ForwardFlat");
    EXPECT_EQ(CurveInterpolationName(CurveInterpolation::KrugerCubic), "KrugerCubic");
    EXPECT_EQ(CurveInterpolationName(CurveInterpolation::NaturalCubic), "NaturalCubic");
}

// ---------------------------------------------------------------------------
// i. average-price quotes: one linear system
// ---------------------------------------------------------------------------

namespace {

/// Months expiring on the last pricing day of their own calendar month, so a
/// calendar month reads exactly one contract month (as of 2026-11-13).
FuturesContract quarterContract(int monthsCount = 3) {
    std::vector<ContractMonth> months = {
        M("2027-01", D(2027, 1, 29), std::nullopt), M("2027-02", D(2027, 2, 26), std::nullopt),
        M("2027-03", D(2027, 3, 31), std::nullopt)};
    months.resize(monthsCount);
    return contract(months);
}

AverageQuote monthQuote(int month, int lastDay, double price) {
    AverageQuote q;
    q.price = price;
    q.periods = {{D(2027, month, 1), D(2027, month, lastDay)}};
    return q;
}

QuantLib::Handle<QuantLib::YieldTermStructure> flatCurve() {
    return QuantLib::Handle<QuantLib::YieldTermStructure>(
        std::make_shared<QuantLib::FlatForward>(kAsOf, 0.04, QuantLib::Actual365Fixed()));
}

/// Paid monthly, PerPeriod, five US business days after each month,
/// discounted at 4 %.
AverageQuote multiMonthQuote(int firstMonth, int lastMonth, double price) {
    AverageQuote q;
    q.price = price;
    q.periods = GeneratePeriods(
        D(2027, firstMonth, 1), QuantLib::Date::endOfMonth(D(2027, lastMonth, 1)),
        QuantLib::Monthly, "periods");
    q.quantityBasis = QuantityBasis::PerPeriod;
    q.paymentCalendar = QuantLib::UnitedStates(QuantLib::UnitedStates::Settlement);
    q.paymentLag = 5;
    q.paymentConvention = QuantLib::Following;
    q.discountCurve = flatCurve();
    return q;
}

CurveBuildInputs withQuotes(std::vector<AverageQuote> quotes) {
    CurveBuildInputs in;
    in.averageQuotes = std::move(quotes);
    in.includeDetails = true;
    return in;
}

double weightOf(const AverageQuoteReport& q, const std::string& month) {
    for (const auto& w : q.weights) {
        if (w.month == month) {
            return w.weight;
        }
    }
    return 0.0;
}

} // namespace

TEST(CommodityAverageQuotes, EquationSpaceRankAndDependence) {
    EquationSpace space(3);
    EXPECT_TRUE(space.add({1.0, 0.0, 0.0}, 1.0).independent);
    EXPECT_TRUE(space.add({0.0, 1.0, 1.0}, 4.0).independent);
    const auto dependent = space.add({2.0, 1.0, 1.0}, 7.0);
    EXPECT_FALSE(dependent.independent);
    EXPECT_NEAR(dependent.implied, 6.0, 1e-12);
    ASSERT_EQ(dependent.combination.size(), 2u);
    EXPECT_NEAR(dependent.combination[0], 2.0, 1e-12);
    EXPECT_NEAR(dependent.combination[1], 1.0, 1e-12);
    EXPECT_EQ(space.rank(), 2u);
    EXPECT_TRUE(space.spans({1.0, 0.0, 0.0}));
    EXPECT_FALSE(space.spans({0.0, 1.0, 0.0}));
    const auto nullSpace = space.nullSpace();
    ASSERT_EQ(nullSpace.size(), 1u);
    EXPECT_NEAR(Dot(nullSpace[0], {0.0, 1.0, 1.0}), 0.0, 1e-12);
    // Minimum-norm solution: x0 = 1, x1 = x2 = 2.
    const auto x = space.solve({1.0, 4.0, 0.0});
    EXPECT_NEAR(x[0], 1.0, 1e-12);
    EXPECT_NEAR(x[1], 2.0, 1e-12);
    EXPECT_NEAR(x[2], 2.0, 1e-12);
    Vector z;
    EXPECT_TRUE(SolveSquare({{2.0, 1.0}, {1.0, 3.0}}, {3.0, 5.0}, z));
    EXPECT_NEAR(z[0], 0.8, 1e-12);
    EXPECT_NEAR(z[1], 1.4, 1e-12);
    EXPECT_FALSE(SolveSquare({{1.0, 2.0}, {2.0, 4.0}}, {1.0, 2.0}, z));
}

TEST(CommodityAverageQuotes, UniqueSquareSystem) {
    const auto r = BuildCommodityCurve(
        quarterContract(),
        withQuotes({monthQuote(1, 31, 50.0), monthQuote(2, 28, 52.0), multiMonthQuote(1, 3, 53.0)}),
        kAsOf);
    const auto& rep = r.report;
    EXPECT_EQ(rep.status, CurveStatus::Unique);
    EXPECT_EQ(rep.unknowns, 3);
    EXPECT_EQ(rep.equations, 3);
    EXPECT_EQ(rep.rank, 3);
    ASSERT_EQ(rep.months.size(), 3u);
    for (const auto& m : rep.months) {
        EXPECT_EQ(m.source, MonthSource::Solved) << m.month;
    }
    EXPECT_NEAR(rep.months[0].price, 50.0, 1e-10);
    EXPECT_NEAR(rep.months[1].price, 52.0, 1e-10);
    // March from the quarter's equation: discounting weights the months.
    const auto& q1 = rep.averageQuotes[2];
    const double wj = weightOf(q1, "2027-01"), wf = weightOf(q1, "2027-02"),
                 wm = weightOf(q1, "2027-03");
    EXPECT_NEAR(wj + wf + wm, 1.0, 1e-12);
    EXPECT_GT(wj, wf);
    EXPECT_GT(wf, wm);
    EXPECT_NEAR(rep.months[2].price, (53.0 - wj * 50.0 - wf * 52.0) / wm, 1e-10);
    EXPECT_LT(rep.maxRepricingDifference, 1e-10);
    for (const auto& q : rep.averageQuotes) {
        EXPECT_EQ(q.status, QuoteStatus::Used);
    }
    // The contract carries the solved prices.
    EXPECT_NEAR(*r.months[2].price, rep.months[2].price, 0.0);
}

TEST(CommodityAverageQuotes, RedundantAndConflict) {
    const auto base = BuildCommodityCurve(
        quarterContract(),
        withQuotes({monthQuote(1, 31, 50.0), monthQuote(2, 28, 52.0), multiMonthQuote(1, 3, 53.0)}),
        kAsOf);
    const double march = base.report.months[2].price;
    const auto redundant = BuildCommodityCurve(
        quarterContract(),
        withQuotes(
            {monthQuote(1, 31, 50.0), monthQuote(2, 28, 52.0), multiMonthQuote(1, 3, 53.0),
             monthQuote(3, 31, march)}),
        kAsOf);
    EXPECT_EQ(redundant.report.averageQuotes[3].status, QuoteStatus::Redundant);
    EXPECT_EQ(redundant.report.rank, 3);
    EXPECT_EQ(redundant.report.equations, 4);
    expectInvalid(
        [&] {
            BuildCommodityCurve(
                quarterContract(),
                withQuotes(
                    {monthQuote(1, 31, 50.0), monthQuote(2, 28, 52.0), multiMonthQuote(1, 3, 53.0),
                     monthQuote(3, 31, 58.0)}),
                kAsOf);
        },
        "average_quotes[3]: conflicts with the other quotes: they imply");
    expectInvalid(
        [&] {
            BuildCommodityCurve(
                quarterContract(),
                withQuotes(
                    {monthQuote(1, 31, 50.0), monthQuote(2, 28, 52.0), multiMonthQuote(1, 3, 53.0),
                     monthQuote(3, 31, 58.0)}),
                kAsOf);
        },
        "(it is determined by average_quotes[0], average_quotes[1], average_quotes[2])");
}

TEST(CommodityAverageQuotes, UnderdeterminedNeedsARule) {
    const auto quotes =
        std::vector<AverageQuote>{monthQuote(1, 31, 50.0), multiMonthQuote(1, 3, 53.0)};
    expectInvalid(
        [&] { BuildCommodityCurve(quarterContract(), withQuotes(quotes), kAsOf); },
        "rule is required: the quotes do not determine 2027-02, 2027-03 (3 months, 2 "
        "independent quotes); give Flat or Smooth");

    auto flatIn = withQuotes(quotes);
    flatIn.rule = CurveRule::Flat;
    const auto flat = BuildCommodityCurve(quarterContract(), flatIn, kAsOf).report;
    auto smoothIn = withQuotes(quotes);
    smoothIn.rule = CurveRule::Smooth;
    const auto smooth = BuildCommodityCurve(quarterContract(), smoothIn, kAsOf).report;
    for (const auto* rep : {&flat, &smooth}) {
        EXPECT_EQ(rep->status, CurveStatus::Underdetermined);
        EXPECT_EQ(rep->rank, 2);
        EXPECT_EQ(rep->months[0].source, MonthSource::Solved);
        EXPECT_EQ(rep->months[1].source, MonthSource::Assumed);
        EXPECT_EQ(rep->months[2].source, MonthSource::Assumed);
        EXPECT_LT(rep->maxRepricingDifference, 1e-10);
        EXPECT_NEAR(rep->months[0].price, 50.0, 1e-10);
    }
    EXPECT_NEAR(flat.months[1].price, flat.months[2].price, 1e-12);
    EXPECT_NEAR(
        smooth.months[0].price - 2.0 * smooth.months[1].price + smooth.months[2].price, 0.0, 1e-10);
    EXPECT_GT(std::fabs(flat.months[2].price - smooth.months[2].price), 0.1);
    EXPECT_EQ(*flat.rule, CurveRule::Flat);
}

TEST(CommodityAverageQuotes, FlatAndSmoothFailures) {
    auto overlapping = withQuotes({multiMonthQuote(1, 2, 51.0), multiMonthQuote(2, 3, 53.0)});
    overlapping.rule = CurveRule::Flat;
    expectInvalid(
        [&] { BuildCommodityCurve(quarterContract(), overlapping, kAsOf); },
        "rule Flat cannot resolve these quotes");
    overlapping.rule = CurveRule::Smooth;
    EXPECT_LT(
        BuildCommodityCurve(quarterContract(), overlapping, kAsOf).report.maxRepricingDifference,
        1e-10);

    auto twoMonths = withQuotes({multiMonthQuote(1, 2, 51.0)});
    twoMonths.rule = CurveRule::Smooth;
    expectInvalid(
        [&] { BuildCommodityCurve(quarterContract(2), twoMonths, kAsOf); },
        "rule Smooth has no unique solution");
    twoMonths.rule = CurveRule::Flat;
    const auto flat = BuildCommodityCurve(quarterContract(2), twoMonths, kAsOf).report;
    EXPECT_NEAR(flat.months[0].price, 51.0, 1e-10);
    EXPECT_NEAR(flat.months[1].price, 51.0, 1e-10);
}

TEST(CommodityAverageQuotes, QuoteValidation) {
    auto missing = multiMonthQuote(1, 3, 53.0);
    missing.quantityBasis.reset();
    expectInvalid(
        [&] { BuildCommodityCurve(quarterContract(), withQuotes({missing}), kAsOf); },
        "average_quotes[0].quantity_basis is required for a quote of 3 periods");
    missing = multiMonthQuote(1, 3, 53.0);
    missing.discountCurve = {};
    expectInvalid(
        [&] { BuildCommodityCurve(quarterContract(), withQuotes({missing}), kAsOf); },
        "average_quotes[0].discounting_curve is required");
    auto badNearby = monthQuote(1, 31, 50.0);
    badNearby.nearby = 0;
    expectInvalid(
        [&] {
            BuildCommodityCurve(
                quarterContract(), withQuotes({monthQuote(2, 28, 52.0), badNearby}), kAsOf);
        },
        "average_quotes[1].nearby must be >= 1");
    // A quote whose days are all before the valuation date.
    AverageQuote past;
    past.price = 50.0;
    past.periods = {{D(2026, 10, 1), D(2026, 10, 30)}};
    expectInvalid(
        [&] { BuildCommodityCurve(quarterContract(), withQuotes({past}), kAsOf); },
        // Paid on 1 November, before the valuation date: nothing left to read.
        "average_quotes[0]: no live month");
    EXPECT_EQ(
        QualifyCurveBuildMessage("curves[2]", "average_quotes[0].price must be finite"),
        "curves[2].average_quotes[0].price must be finite");
    EXPECT_EQ(
        QualifyCurveBuildMessage("curves[0]", "rule is required"), "curves[0].rule is required");
}

TEST(CommodityAverageQuotes, PartlyRealisedQuoteAndNoLiveMonth) {
    // The month 2026-12 last traded on 30 November; November's first days
    // are fixings.
    std::vector<ContractFixing> fixings;
    double fixed = 0.0;
    int fixingDays = 0;
    for (int d = 2; d <= 12; ++d) {
        const Date day = D(2026, 11, d);
        if (QuantLib::WeekendsOnly().isBusinessDay(day)) {
            fixings.push_back({"2026-12", day, 60.0 + d * 0.1});
            fixed += 60.0 + d * 0.1;
            ++fixingDays;
        }
    }
    const FuturesContract withFixings(
        "SYN_CL", PricingCalendar(QuantLib::WeekendsOnly(), {}),
        {M("2026-12", D(2026, 11, 30), std::nullopt), M("2027-01", D(2027, 1, 29), 61.0)}, fixings);
    AverageQuote november;
    november.price = 60.5;
    november.periods = {{D(2026, 11, 1), D(2026, 11, 30)}};
    const auto r = BuildCommodityCurve(withFixings, withQuotes({november}), kAsOf);
    const auto& q = r.report.averageQuotes[0];
    const int days = 21; // weekdays of November 2026
    EXPECT_NEAR(q.constant, fixed / days, 1e-12);
    EXPECT_NEAR(weightOf(q, "2026-12"), double(days - fixingDays) / days, 1e-12);
    EXPECT_EQ(r.report.months[0].source, MonthSource::Solved);
    EXPECT_NEAR(q.constant + weightOf(q, "2026-12") * r.report.months[0].price, 60.5, 1e-10);
    EXPECT_EQ(r.report.months[1].source, MonthSource::Quoted);

    AverageQuote realised;
    realised.price = 60.0;
    realised.periods = {{D(2026, 11, 2), D(2026, 11, 12)}};
    expectInvalid(
        [&] { BuildCommodityCurve(withFixings, withQuotes({realised}), kAsOf); },
        "average_quotes[0]: no live month");
}

TEST(CommodityAverageQuotes, SensitivitiesMatchBumps) {
    const std::vector<double> prices = {50.0, 52.0, 53.0};
    auto build = [&](std::size_t bumped, double h) {
        auto p = prices;
        if (bumped < p.size()) {
            p[bumped] += h;
        }
        return BuildCommodityCurve(
            quarterContract(),
            withQuotes(
                {monthQuote(1, 31, p[0]), monthQuote(2, 28, p[1]), multiMonthQuote(1, 3, p[2])}),
            kAsOf);
    };
    const auto base = build(99, 0.0);
    double largest = 0.0;
    for (std::size_t k = 0; k < 3; ++k) {
        const auto moved = build(k, 1e-3);
        for (std::size_t m = 0; m < 3; ++m) {
            const auto& sens = base.report.months[m].sensitivities;
            ASSERT_EQ(sens.size(), 3u);
            ASSERT_TRUE(sens[k].averageQuote.has_value());
            EXPECT_NEAR(
                (moved.report.months[m].price - base.report.months[m].price) / 1e-3, sens[k].value,
                1e-6);
            largest = std::max(largest, std::fabs(sens[k].value));
        }
    }
    EXPECT_NEAR(*base.report.maxSensitivity, largest, 1e-12);
}

TEST(CommodityAverageQuotes, NoQuotesKeepsTheFill) {
    CurveBuildInputs in;
    in.interpolation = CurveInterpolation::Linear;
    const auto c = strip({59.5, 60.0, std::nullopt, 62.0, 62.5});
    const auto r = BuildCommodityCurve(c, in, kAsOf);
    const auto old = BuildFuturesCurve(c, CurveInterpolation::Linear, false, kAsOf);
    for (std::size_t i = 0; i < r.months.size(); ++i) {
        EXPECT_EQ(r.months[i].price, old.months[i].price);
    }
    EXPECT_EQ(r.report.status, CurveStatus::Unique);
    EXPECT_EQ(r.report.unknowns, 4);
    EXPECT_EQ(r.report.months[2].source, MonthSource::Interpolated);
    EXPECT_EQ(r.report.months[0].source, MonthSource::Quoted);
    EXPECT_FALSE(r.report.maxSensitivity.has_value());
}

TEST(CommodityAverageQuotes, IndependentReferenceStrip) {
    // The swap reference contract (synthetic, as of 2026-11-13) with only
    // 2027-01 priced and one calendar-month quote per month, December 2026 to
    // October 2027: independent reference values.
    const std::vector<Date> holidays = {D(2026, 11, 26), D(2026, 12, 25), D(2027, 1, 1),
                                        D(2027, 1, 18),  D(2027, 2, 15),  D(2027, 3, 26),
                                        D(2027, 5, 31),  D(2027, 7, 5),   D(2027, 9, 6)};
    const std::vector<std::pair<std::string, Date>> ltd = {
        {"2026-11", D(2026, 10, 20)}, {"2026-12", D(2026, 11, 20)}, {"2027-01", D(2026, 12, 21)},
        {"2027-02", D(2027, 1, 20)},  {"2027-03", D(2027, 2, 22)},  {"2027-04", D(2027, 3, 22)},
        {"2027-05", D(2027, 4, 20)},  {"2027-06", D(2027, 5, 20)},  {"2027-07", D(2027, 6, 22)},
        {"2027-08", D(2027, 7, 20)},  {"2027-09", D(2027, 8, 20)},  {"2027-10", D(2027, 9, 21)},
        {"2027-11", D(2027, 10, 20)}, {"2027-12", D(2027, 11, 19)}, {"2028-01", D(2027, 12, 20)}};
    std::vector<ContractMonth> months;
    for (const auto& m : ltd) {
        months.push_back(M(
            m.first, m.second, m.first == "2027-01" ? std::optional<double>(60.4) : std::nullopt));
    }
    const FuturesContract c(
        "SYN_CL", PricingCalendar(QuantLib::WeekendsOnly(), holidays), months, {});
    const std::vector<double> quoted = {
        60.495454545455, 60.810526315789, 61.042105263158, 61.254545454545, 61.436363636364, 61.53,
        61.627272727273, 61.738095238095, 61.831818181818, 61.933333333333, 62.033333333333};
    std::vector<AverageQuote> quotes;
    for (std::size_t k = 0; k < quoted.size(); ++k) {
        const Date first = k == 0 ? D(2026, 12, 1) : D(2027, static_cast<int>(k), 1);
        AverageQuote q;
        q.price = quoted[k];
        q.periods = {{first, QuantLib::Date::endOfMonth(first)}};
        quotes.push_back(q);
    }
    CurveBuildInputs in = withQuotes(quotes);
    in.interpolation = CurveInterpolation::Linear;
    in.extrapolateFlat = true;
    const auto r = BuildCommodityCurve(c, in, kAsOf);
    const std::vector<double> expected = {
        60.4,
        60.70000000000145,
        60.999999999996184,
        61.200000000014924,
        61.39999999995851,
        61.50000000007368,
        61.5999999998281,
        61.700000000459305,
        61.79999999925302,
        61.9000000016,
        61.99999999679902,
        62.10000000640091};
    EXPECT_EQ(r.report.status, CurveStatus::Unique);
    EXPECT_EQ(r.report.unknowns, 12);
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR(*r.months[i + 2].price, expected[i], 1e-6) << ltd[i + 2].first;
    }
    EXPECT_EQ(r.report.months[0].source, MonthSource::Extrapolated); // 2026-12
    EXPECT_NEAR(*r.months[1].price, 60.4, 1e-12);
}

// ---------------------------------------------------------------------------
// j. structured refusals
// ---------------------------------------------------------------------------

namespace {

CurveBuildRefusal refusalOf(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const CurveBuildError& e) {
        return e.refusal();
    }
    ADD_FAILURE() << "expected a CurveBuildError";
    return {};
}

} // namespace

TEST(CommodityCurveBuildRefusal, ConflictCarriesTheNumbers) {
    const auto unique = BuildCommodityCurve(
        quarterContract(),
        withQuotes({monthQuote(1, 31, 50.0), monthQuote(2, 28, 52.0), multiMonthQuote(1, 3, 53.0)}),
        kAsOf);
    const double march = unique.report.months[2].price;
    const auto r = refusalOf([&] {
        BuildCommodityCurve(
            quarterContract(),
            withQuotes(
                {monthQuote(1, 31, 50.0), monthQuote(2, 28, 52.0), multiMonthQuote(1, 3, 53.0),
                 monthQuote(3, 31, 58.0)}),
            kAsOf);
    });
    EXPECT_EQ(r.reason, CurveBuildRefusal::Reason::QuoteConflict);
    EXPECT_EQ(r.averageQuote, std::optional<std::size_t>(3));
    EXPECT_NEAR(*r.impliedValue, march, 1e-10);
    EXPECT_EQ(*r.quotedValue, 58.0);
    EXPECT_EQ(*r.gap, 58.0 - *r.impliedValue);
    ASSERT_EQ(r.dependsOn.size(), 3u);
    for (std::size_t k = 0; k < 3; ++k) {
        EXPECT_EQ(r.dependsOn[k].averageQuote, std::optional<std::size_t>(k));
        EXPECT_FALSE(r.dependsOn[k].month.has_value());
    }
    EXPECT_FALSE(r.rule.has_value());
    EXPECT_EQ(CurveBuildRefusalReasonName(r.reason), "QuoteConflict");
}

TEST(CommodityCurveBuildRefusal, EveryReason) {
    using Reason = CurveBuildRefusal::Reason;
    const auto quotes =
        std::vector<AverageQuote>{monthQuote(1, 31, 50.0), multiMonthQuote(1, 3, 53.0)};
    auto r = refusalOf([&] { BuildCommodityCurve(quarterContract(), withQuotes(quotes), kAsOf); });
    EXPECT_EQ(r.reason, Reason::RuleRequired);
    EXPECT_EQ(r.months, (std::vector<std::string>{"2027-02", "2027-03"}));

    auto overlapping = withQuotes({multiMonthQuote(1, 2, 51.0), multiMonthQuote(2, 3, 53.0)});
    overlapping.rule = CurveRule::Flat;
    r = refusalOf([&] { BuildCommodityCurve(quarterContract(), overlapping, kAsOf); });
    EXPECT_EQ(r.reason, Reason::FlatCannotResolve);
    EXPECT_EQ(r.rule, std::optional<CurveRule>(CurveRule::Flat));
    EXPECT_FALSE(r.months.empty());

    auto two = withQuotes({multiMonthQuote(1, 2, 51.0)});
    two.rule = CurveRule::Smooth;
    r = refusalOf([&] { BuildCommodityCurve(quarterContract(2), two, kAsOf); });
    EXPECT_EQ(r.reason, Reason::SmoothNotUnique);
    EXPECT_EQ(r.months, (std::vector<std::string>{"2027-01", "2027-02"}));

    r = refusalOf([&] {
        BuildFuturesCurve(
            strip({std::nullopt, 60.0, 61.0, 62.0, std::nullopt}), CurveInterpolation::Linear,
            false, kAsOf);
    });
    EXPECT_EQ(r.reason, Reason::MonthOutsideRange);
    EXPECT_EQ(r.month, std::optional<std::string>("2026-12"));
    EXPECT_EQ(r.months, (std::vector<std::string>{"2026-12", "2027-04"}));

    r = refusalOf([&] {
        BuildFuturesCurve(
            strip({-1.0, 60.0, std::nullopt, 62.0, 62.5}), CurveInterpolation::LogLinear, false,
            kAsOf);
    });
    EXPECT_EQ(r.reason, Reason::NonPositivePrice);
    EXPECT_EQ(r.month, std::optional<std::string>("2026-12"));

    r = refusalOf([&] {
        BuildFuturesCurve(
            strip({60.0, std::nullopt, 61.0, 62.0, 62.5}), std::nullopt, false, kAsOf);
    });
    EXPECT_EQ(r.reason, Reason::InterpolationRequired);
    EXPECT_EQ(r.months, (std::vector<std::string>{"2027-01"}));

    AverageQuote past;
    past.price = 50.0;
    past.periods = {{D(2026, 10, 1), D(2026, 10, 30)}};
    r = refusalOf([&] { BuildCommodityCurve(quarterContract(), withQuotes({past}), kAsOf); });
    EXPECT_EQ(r.reason, Reason::QuoteHasNoLiveMonth);
    EXPECT_EQ(r.averageQuote, std::optional<std::size_t>(0));
}

TEST(CommodityCurveBuildRefusal, InvalidQuoteValuesNameTheirField) {
    auto missing = multiMonthQuote(1, 3, 53.0);
    missing.quantityBasis.reset();
    auto r =
        refusalOf([&] { BuildCommodityCurve(quarterContract(), withQuotes({missing}), kAsOf); });
    EXPECT_EQ(r.reason, CurveBuildRefusal::Reason::InvalidInput);
    EXPECT_EQ(r.path, "average_quotes[0].quantity_basis");
    EXPECT_EQ(r.averageQuote, std::optional<std::size_t>(0));
    auto badNearby = monthQuote(1, 31, 50.0);
    badNearby.nearby = 0;
    r = refusalOf([&] {
        BuildCommodityCurve(
            quarterContract(), withQuotes({monthQuote(2, 28, 52.0), badNearby}), kAsOf);
    });
    EXPECT_EQ(r.path, "average_quotes[1].nearby");
    EXPECT_EQ(r.averageQuote, std::optional<std::size_t>(1));
    // The paths of the other refusals, relative to the build.
    const auto quotes =
        std::vector<AverageQuote>{monthQuote(1, 31, 50.0), multiMonthQuote(1, 3, 53.0)};
    r = refusalOf([&] { BuildCommodityCurve(quarterContract(), withQuotes(quotes), kAsOf); });
    EXPECT_EQ(r.path, "rule");
    // The contract's own rules keep their field on the error, unchanged
    // message.
    try {
        contract({M("2027-01", D(2026, 12, 21), 60.0), M("2027-02", D(2026, 12, 21), 61.0)});
        ADD_FAILURE() << "expected an invalid argument";
    } catch (const QuantraInvalidArgument& e) {
        EXPECT_EQ(e.path(), "months[1].last_trade_date");
        EXPECT_TRUE(e.details().empty());
    }
}

TEST(CommodityCurveBuildRefusal, WireShape) {
    CurveBuildRefusal r;
    r.reason = CurveBuildRefusal::Reason::QuoteConflict;
    r.averageQuote = 3;
    r.impliedValue = 57.125;
    r.quotedValue = 58.0;
    r.gap = 0.1 + 0.2;
    r.dependsOn = {{std::string("2027-01"), std::nullopt}, {std::nullopt, std::size_t(1)}};
    EXPECT_EQ(
        quantra::CurveBuildRefusalJson(r, 2),
        "{\"reason\": \"QuoteConflict\", \"curve\": 2, \"average_quote\": 3, \"implied_value\": "
        "57.125, \"quoted_value\": 58, \"gap\": 0.30000000000000004, "
        "\"depends_on\": [{\"month\": \"2027-01\"}, {\"average_quote\": 1}]}");
    CurveBuildRefusal rule;
    rule.reason = CurveBuildRefusal::Reason::SmoothNotUnique;
    rule.months = {"2027-01", "2027-02"};
    rule.rule = CurveRule::Smooth;
    EXPECT_EQ(
        quantra::CurveBuildRefusalJson(rule, 0),
        "{\"reason\": \"SmoothNotUnique\", \"curve\": 0, \"months\": [\"2027-01\", \"2027-02\"], "
        "\"rule\": \"Smooth\"}");
    // The error that carries it.
    const QuantraInvalidArgument e("message", "{\"reason\": \"RuleRequired\"}");
    EXPECT_EQ(std::string(e.what()), "message");
    EXPECT_EQ(e.details(), "{\"reason\": \"RuleRequired\"}");
    EXPECT_TRUE(QuantraInvalidArgument("plain").details().empty());
}

// ---------------------------------------------------------------------------
// k. spot prices from dated points and average quotes
// ---------------------------------------------------------------------------

namespace {

SpotCurveSpec spotSpec(
    SpotInterpolation interpolation,
    const std::vector<std::pair<Date, std::optional<double>>>& points,
    bool extrapolateFlat = false) {
    SpotCurveSpec spec{"SYN_SPOT",
                       PricingCalendar(QuantLib::WeekendsOnly(), {}),
                       interpolation,
                       extrapolateFlat,
                       {},
                       {}};
    for (const auto& p : points) {
        spec.points.push_back({p.first, p.second});
    }
    return spec;
}

AverageQuote spotQuote(Date start, Date end, double price) {
    AverageQuote q;
    q.price = price;
    q.periods = {{start, end}};
    return q;
}

} // namespace

TEST(CommoditySpotBuild, BackwardFlatMonthQuotesAreTheirMonthEnds) {
    // Every December day reads the 31 December point, every January day the
    // 31 January point: each quote is its point.
    const auto spec = spotSpec(
        SpotInterpolation::BackwardFlat,
        {{D(2026, 11, 30), 60.0}, {D(2026, 12, 31), std::nullopt}, {D(2027, 1, 31), std::nullopt}});
    auto in = withQuotes(
        {spotQuote(D(2026, 12, 1), D(2026, 12, 31), 60.5),
         spotQuote(D(2027, 1, 1), D(2027, 1, 31), 61.0)});
    const auto r = BuildSpotCurve(spec, in, kAsOf);
    EXPECT_EQ(r.report.status, CurveStatus::Unique);
    ASSERT_EQ(r.points.size(), 3u);
    EXPECT_NEAR(*r.points[1].price, 60.5, 1e-12);
    EXPECT_NEAR(*r.points[2].price, 61.0, 1e-12);
    EXPECT_EQ(r.report.points[0].source, MonthSource::Quoted);
    EXPECT_EQ(r.report.points[1].source, MonthSource::Solved);
    ASSERT_EQ(r.report.averageQuotes[0].weights.size(), 1u);
    EXPECT_EQ(r.report.averageQuotes[0].weights[0].date, "2026-12-31");
    EXPECT_TRUE(r.report.averageQuotes[0].weights[0].month.empty());
    EXPECT_TRUE(r.report.months.empty());
    EXPECT_LT(r.report.maxRepricingDifference, 1e-10);
}

TEST(CommoditySpotBuild, LinearChainsThroughThePinnedPoint) {
    // December days lie between 30 November (pinned) and 31 December: the
    // quote is a weighted average of the two, and solving it back gives the
    // point; repricing is exact.
    const auto spec = spotSpec(
        SpotInterpolation::Linear, {{D(2026, 11, 30), 60.0}, {D(2026, 12, 31), std::nullopt}});
    const auto r =
        BuildSpotCurve(spec, withQuotes({spotQuote(D(2026, 12, 1), D(2026, 12, 31), 60.5)}), kAsOf);
    const auto& w = r.report.averageQuotes[0].weights;
    ASSERT_EQ(w.size(), 2u);
    EXPECT_NEAR(w[0].weight + w[1].weight, 1.0, 1e-14);
    EXPECT_NEAR(w[0].weight * 60.0 + w[1].weight * *r.points[1].price, 60.5, 1e-10);
}

TEST(CommoditySpotBuild, SmoothUsesDividedDifferencesOnTheDateAxis) {
    // One quarter quote over three month-end points, the December point
    // pinned: Smooth makes the second divided differences zero, so the four
    // points lie on one line in time even though the gaps differ (31, 28, 31
    // days).
    const auto spec = spotSpec(
        SpotInterpolation::BackwardFlat, {{D(2026, 12, 31), 60.4},
                                          {D(2027, 1, 31), std::nullopt},
                                          {D(2027, 2, 28), std::nullopt},
                                          {D(2027, 3, 31), std::nullopt}});
    auto in = withQuotes({spotQuote(D(2027, 1, 1), D(2027, 3, 31), 61.0)});
    in.rule = CurveRule::Smooth;
    const auto r = BuildSpotCurve(spec, in, kAsOf);
    std::vector<double> t, p;
    for (const auto& pt : r.points) {
        t.push_back(static_cast<double>(pt.date - kAsOf) / 365.0);
        p.push_back(*pt.price);
    }
    const double s0 = (p[1] - p[0]) / (t[1] - t[0]);
    for (std::size_t i = 1; i + 1 < p.size(); ++i) {
        EXPECT_NEAR((p[i + 1] - p[i]) / (t[i + 1] - t[i]), s0, 1e-9);
    }
    EXPECT_EQ(r.report.status, CurveStatus::Underdetermined);
    EXPECT_EQ(r.report.points[1].source, MonthSource::Assumed);
    in.rule.reset();
    const auto refusal = refusalOf([&] { BuildSpotCurve(spec, in, kAsOf); });
    EXPECT_EQ(refusal.reason, CurveBuildRefusal::Reason::RuleRequired);
    EXPECT_EQ(refusal.dates, (std::vector<std::string>{"2027-01-31", "2027-02-28", "2027-03-31"}));
    EXPECT_TRUE(refusal.months.empty());
}

TEST(CommoditySpotBuild, RefusalsCarryDates) {
    // A quote day after the last point without extrapolate_flat.
    auto spec = spotSpec(
        SpotInterpolation::Linear, {{D(2026, 11, 30), 60.0}, {D(2027, 2, 28), std::nullopt}});
    auto r = refusalOf([&] {
        BuildSpotCurve(spec, withQuotes({spotQuote(D(2027, 2, 1), D(2027, 3, 31), 60.5)}), kAsOf);
    });
    EXPECT_EQ(r.reason, CurveBuildRefusal::Reason::DateOutsideRange);
    EXPECT_EQ(r.averageQuote, std::optional<std::size_t>(0));
    EXPECT_EQ(r.date, std::optional<std::string>("2027-03-01"));
    // A point to fill after the last determined one.
    spec = spotSpec(
        SpotInterpolation::Linear,
        {{D(2026, 11, 30), 60.0}, {D(2026, 12, 31), std::nullopt}, {D(2027, 1, 31), std::nullopt}});
    r = refusalOf([&] {
        BuildSpotCurve(spec, withQuotes({spotQuote(D(2026, 12, 1), D(2026, 12, 31), 60.2)}), kAsOf);
    });
    EXPECT_EQ(r.reason, CurveBuildRefusal::Reason::DateOutsideRange);
    EXPECT_EQ(r.path, "spot.points[2]");
    EXPECT_EQ(r.dates, (std::vector<std::string>{"2027-01-31"}));
    // A conflict names the direct point it depends on by date.
    spec = spotSpec(
        SpotInterpolation::BackwardFlat, {{D(2026, 11, 30), 60.0}, {D(2026, 12, 31), 60.5}});
    r = refusalOf([&] {
        BuildSpotCurve(
            spec, withQuotes({spotQuote(D(2026, 12, 1), D(2026, 12, 31), 60.75)}), kAsOf);
    });
    EXPECT_EQ(r.reason, CurveBuildRefusal::Reason::QuoteConflict);
    ASSERT_EQ(r.dependsOn.size(), 1u);
    EXPECT_EQ(r.dependsOn[0].date, std::optional<std::string>("2026-12-31"));
    EXPECT_NEAR(*r.gap, 0.25, 1e-12);
}

// ---------------------------------------------------------------------------
// l. basis: base price + basis
// ---------------------------------------------------------------------------

namespace {

/// The base: three live months priced, one expired month with its final
/// fixing (2026-10, last traded 2026-09-22 at 58.6).
std::shared_ptr<const FuturesContract> basisBase() {
    return std::make_shared<const FuturesContract>(
        "SYN_CL", PricingCalendar(QuantLib::WeekendsOnly(), {}),
        std::vector<ContractMonth>{
            M("2026-10", D(2026, 9, 22), std::nullopt), M("2026-12", D(2026, 11, 20), 60.0),
            M("2027-01", D(2026, 12, 21), 60.4), M("2027-02", D(2027, 1, 20), 60.7),
            M("2027-03", D(2027, 2, 22), 61.0), M("2027-04", D(2027, 3, 22), 61.2)},
        std::vector<ContractFixing>{{"2026-10", D(2026, 9, 22), 58.6}});
}

FuturesContract basisContract() {
    return contract(
        {M("2026-12", D(2026, 11, 24), std::nullopt), M("2027-01", D(2026, 12, 23), std::nullopt),
         M("2027-02", D(2027, 1, 22), std::nullopt), M("2027-03", D(2027, 2, 24), std::nullopt)});
}

CurveBuildInputs basisInputs(std::vector<BasisQuote> quotes, int offset = 0) {
    CurveBuildInputs in;
    BasisInputs b;
    b.base = basisBase();
    b.baseCurve = 1;
    b.baseMonthOffset = offset;
    b.quotes = std::move(quotes);
    in.basis = b;
    in.interpolation = CurveInterpolation::Linear;
    in.extrapolateFlat = true;
    return in;
}

} // namespace

TEST(CommodityBasisBuild, SameMonthAddsTheBasis) {
    const auto r = BuildCommodityCurve(
        basisContract(),
        basisInputs({{"2026-12", 0.8}, {"2027-01", 0.75}, {"2027-02", 0.7}, {"2027-03", 0.72}}),
        kAsOf);
    const std::vector<double> want = {60.8, 61.15, 61.4, 61.72};
    for (std::size_t i = 0; i < want.size(); ++i) {
        EXPECT_NEAR(*r.months[i].price, want[i], 1e-12);
        EXPECT_EQ(r.report.months[i].source, MonthSource::Basis);
        EXPECT_NEAR(*r.report.months[i].basePrice + *r.report.months[i].basis, want[i], 1e-12);
    }
}

TEST(CommodityBasisBuild, GapsAreFilledOnTheBasis) {
    // February: basis linear in time between January (0.75, 23 Dec) and
    // March (0.72, 24 Feb), 30 of 63 days, then the base 60.7 added back.
    const auto r = BuildCommodityCurve(
        basisContract(), basisInputs({{"2027-01", 0.75}, {"2027-03", 0.72}}), kAsOf);
    EXPECT_NEAR(*r.months[2].price, 60.7 + 0.75 - 0.03 * 30.0 / 63.0, 1e-12);
    EXPECT_EQ(r.report.months[2].source, MonthSource::Interpolated);
    // December: before the first quote, held flat on the basis.
    EXPECT_NEAR(*r.months[0].price, 60.0 + 0.75, 1e-12);
    EXPECT_EQ(r.report.months[0].source, MonthSource::Extrapolated);
}

TEST(CommodityBasisBuild, OffsetReadsAnExpiredBaseMonthFromItsFixing) {
    // December 2026 with an offset of 2 reads the October base month, expired
    // on 22 September: its final fixing 58.6.
    const auto r = BuildCommodityCurve(
        contract({M("2026-12", D(2026, 11, 24), std::nullopt)}), basisInputs({{"2026-12", 0.8}}, 2),
        kAsOf);
    EXPECT_NEAR(*r.months[0].price, 58.6 + 0.8, 1e-12);
    // Every live month needs its base price: January would read November,
    // which the base does not list.
    const auto refusal = refusalOf(
        [&] { BuildCommodityCurve(basisContract(), basisInputs({{"2026-12", 0.8}}, 2), kAsOf); });
    EXPECT_EQ(refusal.reason, CurveBuildRefusal::Reason::BaseNotDetermined);
    EXPECT_EQ(refusal.baseMonth, std::optional<std::string>("2026-11"));
    EXPECT_EQ(refusal.baseCurve, std::optional<std::size_t>(1));
}

TEST(CommodityBasisBuild, Refusals) {
    auto withPrice = basisContract().months();
    withPrice[1].price = 61.0;
    auto r = refusalOf(
        [&] { BuildCommodityCurve(contract(withPrice), basisInputs({{"2027-01", 0.75}}), kAsOf); });
    EXPECT_EQ(r.reason, CurveBuildRefusal::Reason::BasisAndPrice);
    EXPECT_EQ(r.path, "basis.quotes[0]");
    r = refusalOf([&] {
        BuildCommodityCurve(basisContract(), basisInputs({{"2026-12", 0.8}}), D(2026, 12, 1));
    });
    EXPECT_EQ(r.reason, CurveBuildRefusal::Reason::BasisOnExpiredMonth);
    auto average = basisInputs({{"2027-01", 0.75}});
    average.basis->basePrice = BasisBasePrice::MonthAverage;
    r = refusalOf([&] { BuildCommodityCurve(basisContract(), average, kAsOf); });
    EXPECT_EQ(r.reason, CurveBuildRefusal::Reason::InvalidInput);
    EXPECT_EQ(r.path, "basis.base_nearby");
    r = refusalOf(
        [&] { BuildCommodityCurve(basisContract(), basisInputs({{"2030-01", 0.75}}), kAsOf); });
    EXPECT_EQ(r.path, "basis.quotes[0].month");
}

TEST(CommodityBasisBuild, MonthAverageUsesTheSwapAverage) {
    // The base front month, roll 0, averaged over each calendar month.
    auto in = basisInputs({{"2027-02", 0.7}});
    in.basis->basePrice = BasisBasePrice::MonthAverage;
    in.basis->baseNearby = 1;
    in.basis->baseRollDaysBeforeExpiry = 0;
    const auto avg = contract(
        {M("2027-01", D(2027, 1, 29), std::nullopt), M("2027-02", D(2027, 2, 26), std::nullopt)});
    const auto r = BuildCommodityCurve(avg, in, kAsOf);
    // February 2027: 20 weekdays, 1-22 February (16 days) on March (61.0),
    // 23-26 February (4 days) on April (61.2).
    EXPECT_NEAR(*r.months[1].price, (16 * 61.0 + 4 * 61.2) / 20.0 + 0.7, 1e-12);
    // January 2027: 21 weekdays, 1-20 January (14 days) on February (60.7),
    // 21-29 January (7 days) on March (61.0).
    EXPECT_NEAR(*r.report.months[0].basePrice, (14 * 60.7 + 7 * 61.0) / 21.0, 1e-12);
}
