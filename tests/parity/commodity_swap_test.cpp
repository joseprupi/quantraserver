// Commodity swap core library (src/commodities): periods, pricing days,
// contract selection and roll, spot interpolation, observations, the swap
// arithmetic, fair values and deltas.
//
//  a. periods: calendar-aligned generation (month, quarter, half-year, year,
//     once), leap February, every validation error;
//  b. pricing days: calendar business days minus own holidays; empty floating
//     period refused;
//  c. contract selection: roll 0 reads the expiring month on its last trading
//     date, roll 2 switches two pricing days earlier, own holidays move the
//     roll date, nearby 2, every validation error;
//  d. spot price: Linear / BackwardFlat / ForwardFlat, flat extrapolation or
//     refusal, log interpolations refused;
//  e. reference values (synthetic data): three monthly swaps against a
//     futures strip (roll 0 and roll 2) and a spot price, as of 2026-11-13:
//     NPV, fair fixed price, every period's pricing days, payment date,
//     average and discount, every day's contract month; the pricing days are
//     asserted equal to the reference observation dates;
//  f. properties: payer = -receiver, NPV 0 at the fair fixed price and at the
//     fair spread, a fully realised period, deltas vs central finite
//     differences, quantity per pricing day vs per period, a payment on the
//     valuation date, a paid period without pricing days, swap-level
//     validation errors and their field paths;
//  g. the YYYY-MM month check.
#include "commodity_swap.h"

#include "calendar_overrides.h"
#include "commodity_futures_contract.h"
#include "commodity_observation.h"
#include "commodity_periods.h"
#include "commodity_pricing_days.h"
#include "commodity_spot_price.h"
#include "date_convert.h"
#include "error.h"

#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/termstructures/yield/discountcurve.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/calendars/weekendsonly.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace quantra::commodities;
using QuantLib::Date;

namespace {

Date D(int y, int m, int d) {
    return Date(d, static_cast<QuantLib::Month>(m), y);
}

void expectInvalid(const std::function<void()>& fn, const std::string& fragment) {
    try {
        fn();
        ADD_FAILURE() << "expected an invalid argument containing '" << fragment << "'";
    } catch (const QuantraInvalidArgument& e) {
        EXPECT_NE(std::string(e.what()).find(fragment), std::string::npos)
            << "message '" << e.what() << "' lacks '" << fragment << "'";
    }
}

// ---------------------------------------------------------------------------
// Reference case (synthetic data)
// ---------------------------------------------------------------------------

const Date kAsOf = D(2026, 11, 13);

/// Weekdays that are not pricing days of the reference exchange calendar
/// between 2026-11-01 and 2027-10-31.
const std::vector<Date> kReferenceHolidays = {D(2026, 11, 26), D(2026, 12, 25), D(2027, 1, 1),
                                              D(2027, 1, 18),  D(2027, 2, 15),  D(2027, 3, 26),
                                              D(2027, 5, 31),  D(2027, 7, 5),   D(2027, 9, 6)};

QuantLib::Handle<QuantLib::YieldTermStructure> referenceDiscountCurve() {
    // Dated discount factors, log-linear in Actual/365 (Fixed) time.
    const std::vector<Date> dates = {
        kAsOf,          D(2026, 12, 13), D(2027, 1, 13),  D(2027, 2, 12),  D(2027, 3, 15),
        D(2027, 4, 14), D(2027, 5, 14),  D(2027, 6, 14),  D(2027, 7, 14),  D(2027, 8, 14),
        D(2027, 9, 13), D(2027, 10, 14), D(2027, 11, 13), D(2027, 12, 13), D(2028, 1, 13)};
    const std::vector<double> dfs = {
        1.0,
        0.996717727240435,
        0.993337362941161,
        0.990076958773721,
        0.986719116614900,
        0.983480435337093,
        0.980252384294621,
        0.976927862141973,
        0.973721318432005,
        0.970418946363854,
        0.967233766690838,
        0.963953397129210,
        0.960789439152323,
        0.957635866148516,
        0.954388047829286};
    return QuantLib::Handle<QuantLib::YieldTermStructure>(
        std::make_shared<QuantLib::InterpolatedDiscountCurve<QuantLib::LogLinear>>(
            dates, dfs, QuantLib::Actual365Fixed()));
}

std::vector<ContractMonth> referenceMonths() {
    return {{"2026-11", D(2026, 10, 20), std::nullopt}, {"2026-12", D(2026, 11, 20), 60.0},
            {"2027-01", D(2026, 12, 21), 60.4},         {"2027-02", D(2027, 1, 20), 60.7},
            {"2027-03", D(2027, 2, 22), 61.0},          {"2027-04", D(2027, 3, 22), 61.2},
            {"2027-05", D(2027, 4, 20), 61.4},          {"2027-06", D(2027, 5, 20), 61.5},
            {"2027-07", D(2027, 6, 22), 61.6},          {"2027-08", D(2027, 7, 20), 61.7},
            {"2027-09", D(2027, 8, 20), 61.8},          {"2027-10", D(2027, 9, 21), 61.9},
            {"2027-11", D(2027, 10, 20), 62.0},         {"2027-12", D(2027, 11, 19), 62.1},
            {"2028-01", D(2027, 12, 20), 62.2}};
}

std::vector<ContractFixing> referenceContractFixings() {
    const std::vector<std::pair<Date, double>> f = {
        {D(2026, 11, 2), 59.0},  {D(2026, 11, 3), 59.25},  {D(2026, 11, 4), 59.5},
        {D(2026, 11, 5), 59.75}, {D(2026, 11, 6), 60.0},   {D(2026, 11, 9), 60.25},
        {D(2026, 11, 10), 60.5}, {D(2026, 11, 11), 60.25}, {D(2026, 11, 12), 60.0}};
    std::vector<ContractFixing> out;
    for (const auto& x : f) {
        out.push_back({"2026-12", x.first, x.second});
    }
    return out;
}

std::vector<SpotPoint> referenceSpotPoints() {
    std::vector<SpotPoint> out;
    double price = 59.5;
    for (int k = 0; k < 14; ++k) {
        const int month = 11 + k;
        out.push_back({D(2026 + (month - 1) / 12, (month - 1) % 12 + 1, 15), price});
        price += 0.25;
    }
    return out;
}

std::vector<SpotFixing> referenceSpotFixings() {
    const std::vector<Date> days = {D(2026, 11, 2),  D(2026, 11, 3),  D(2026, 11, 4),
                                    D(2026, 11, 5),  D(2026, 11, 6),  D(2026, 11, 9),
                                    D(2026, 11, 10), D(2026, 11, 11), D(2026, 11, 12)};
    std::vector<SpotFixing> out;
    for (std::size_t i = 0; i < days.size(); ++i) {
        out.push_back({days[i], 59.0 + 0.1 * static_cast<double>(i)});
    }
    return out;
}

/// The reference exchange calendar as a US calendar: NYSE, with 2027-06-18
/// (the Friday before a Saturday holiday, open at the reference exchange)
/// made a business day by a request-level calendar override. Holds the
/// override for its lifetime.
struct ReferenceCalendarScope {
    quantra::CalendarOverridesGuard guard;
    ReferenceCalendarScope() {
        quantra::applyCalendarOverrides(
            {{QuantLib::UnitedStates(QuantLib::UnitedStates::NYSE), {}, {D(2027, 6, 18)}}});
    }
};

PricingCalendar referencePricingCalendar() {
    return PricingCalendar(QuantLib::UnitedStates(QuantLib::UnitedStates::NYSE), {});
}

/// The same pricing days from a weekdays-only calendar and own holidays.
PricingCalendar ownHolidaysPricingCalendar() {
    return PricingCalendar(QuantLib::WeekendsOnly(), kReferenceHolidays);
}

std::shared_ptr<const FuturesContract> referenceContract(const PricingCalendar& cal) {
    return std::make_shared<FuturesContract>(
        "SYN_CL", cal, referenceMonths(), referenceContractFixings());
}

std::shared_ptr<const SpotPrice> referenceSpot(const PricingCalendar& cal) {
    return std::make_shared<SpotPrice>(
        "SYN_SPOT", cal, SpotInterpolation::Linear, true, referenceSpotPoints(),
        referenceSpotFixings());
}

CommoditySwapLeg monthlyLeg(PayReceive side, LegPrice price) {
    CommoditySwapLeg leg;
    leg.payReceive = side;
    leg.periods = GeneratePeriods(D(2026, 11, 1), D(2027, 10, 31), QuantLib::Monthly, "periods");
    leg.quantity.value = 10000.0;
    leg.quantity.basis = QuantityBasis::PerPeriod;
    leg.price = std::move(price);
    leg.paymentCalendar = QuantLib::UnitedStates(QuantLib::UnitedStates::Settlement);
    leg.paymentLag = 5;
    leg.paymentConvention = QuantLib::Following;
    leg.discountCurve = referenceDiscountCurve();
    return leg;
}

CommoditySwapLeg fixedPayLeg(double k = 61.0) {
    FixedPrice fp;
    fp.price = k;
    return monthlyLeg(PayReceive::Pay, fp);
}

CommoditySwapLeg futuresLeg(
    std::shared_ptr<const FuturesContract> contract,
    int roll,
    PayReceive side = PayReceive::Receive,
    double spread = 0.0) {
    FuturesFloatingPrice fl;
    fl.contract = std::move(contract);
    fl.nearby = 1;
    fl.rollDaysBeforeExpiry = roll;
    fl.spread = spread;
    return monthlyLeg(side, fl);
}

CommoditySwapLeg spotLeg(
    std::shared_ptr<const SpotPrice> spot,
    PayReceive side = PayReceive::Receive,
    double spread = 0.0) {
    SpotFloatingPrice sp;
    sp.spot = std::move(spot);
    sp.spread = spread;
    return monthlyLeg(side, sp);
}

const std::vector<int> kPricingDays = {20, 22, 19, 19, 22, 22, 20, 22, 21, 22, 21, 21};
const std::vector<Date> kPayments = {D(2026, 12, 7), D(2027, 1, 8), D(2027, 2, 5),  D(2027, 3, 5),
                                     D(2027, 4, 7),  D(2027, 5, 7), D(2027, 6, 7),  D(2027, 7, 8),
                                     D(2027, 8, 6),  D(2027, 9, 8), D(2027, 10, 7), D(2027, 11, 5)};
const std::vector<double> kDiscounts = {0.9973733188, 0.9938818065, 0.9908367613, 0.9878010454,
                                        0.9842351755, 0.9810046472, 0.9776775738, 0.9743617841,
                                        0.9712700977, 0.967763903,  0.9646931519, 0.9616321445};
const std::vector<double> kAvgRoll0 = {
    60.025, 60.495454545455, 60.810526315789, 61.042105263158, 61.254545454545, 61.436363636364,
    61.53,  61.627272727273, 61.738095238095, 61.831818181818, 61.933333333333, 62.033333333333};
const std::vector<double> kAvgRoll2 = {
    60.065, 60.522727272727, 60.842105263158, 61.063157894737, 61.272727272727, 61.445454545455,
    61.54,  61.636363636364, 61.747619047619, 61.840909090909, 61.942857142857, 62.042857142857};
const std::vector<double> kAvgSpot = {59.48375,        59.753775659824, 60.011460101868,
                                      60.23976776619,  60.498127880184, 60.759604105572,
                                      61.003158602151, 61.253641251222, 61.511866359447,
                                      61.757697947214, 62.010394265233, 62.255261136713};

/// First pricing day on each contract month (reference observations).
const std::vector<std::pair<Date, std::string>> kChangesRoll0 = {
    {D(2026, 11, 2), "2026-12"}, {D(2026, 11, 23), "2027-01"}, {D(2026, 12, 22), "2027-02"},
    {D(2027, 1, 21), "2027-03"}, {D(2027, 2, 23), "2027-04"},  {D(2027, 3, 23), "2027-05"},
    {D(2027, 4, 21), "2027-06"}, {D(2027, 5, 21), "2027-07"},  {D(2027, 6, 23), "2027-08"},
    {D(2027, 7, 21), "2027-09"}, {D(2027, 8, 23), "2027-10"},  {D(2027, 9, 22), "2027-11"},
    {D(2027, 10, 21), "2027-12"}};
const std::vector<std::pair<Date, std::string>> kChangesRoll2 = {
    {D(2026, 11, 2), "2026-12"}, {D(2026, 11, 19), "2027-01"}, {D(2026, 12, 18), "2027-02"},
    {D(2027, 1, 19), "2027-03"}, {D(2027, 2, 19), "2027-04"},  {D(2027, 3, 19), "2027-05"},
    {D(2027, 4, 19), "2027-06"}, {D(2027, 5, 19), "2027-07"},  {D(2027, 6, 21), "2027-08"},
    {D(2027, 7, 19), "2027-09"}, {D(2027, 8, 19), "2027-10"},  {D(2027, 9, 20), "2027-11"},
    {D(2027, 10, 19), "2027-12"}};

/// The month `contract` reads on `day` for `nearby` and a roll of `roll`
/// pricing days.
std::string monthRead(const FuturesContract& contract, Date day, int nearby, int roll) {
    return contract.months()[contract.selectMonth(day, nearby, contract.rollDates(roll))].month;
}

std::string expectedMonth(const std::vector<std::pair<Date, std::string>>& changes, Date day) {
    std::string month;
    for (const auto& c : changes) {
        if (c.first <= day) month = c.second;
    }
    return month;
}

void checkReferenceSwap(
    const CommoditySwapResult& r,
    double npv,
    double par,
    const std::vector<double>& averages,
    const std::vector<std::pair<Date, std::string>>* changes) {
    EXPECT_NEAR(r.npv, npv, 1e-5);
    ASSERT_TRUE(r.fairFixedPrice.has_value());
    EXPECT_NEAR(*r.fairFixedPrice, par, 1e-9);
    EXPECT_FALSE(r.fairSpread.has_value());
    const auto& fixedFlows = r.legs[0].flows;
    const auto& floatFlows = r.legs[1].flows;
    ASSERT_EQ(fixedFlows.size(), 12u);
    ASSERT_EQ(floatFlows.size(), 12u);

    // Pricing days = the reference observation dates: every weekday of the
    // swap minus the reference holidays.
    std::vector<Date> expectedDays;
    for (Date d = D(2026, 11, 1); d <= D(2027, 10, 31); ++d) {
        const bool weekend = d.weekday() == QuantLib::Saturday || d.weekday() == QuantLib::Sunday;
        bool holiday = false;
        for (const auto& h : kReferenceHolidays)
            holiday = holiday || h == d;
        if (!weekend && !holiday) expectedDays.push_back(d);
    }
    std::vector<Date> days;
    for (std::size_t i = 0; i < 12; ++i) {
        const auto& f = floatFlows[i];
        EXPECT_EQ(f.pricingDays, kPricingDays[i]) << "period " << i;
        EXPECT_EQ(f.paymentDate, kPayments[i]) << "period " << i;
        EXPECT_EQ(fixedFlows[i].paymentDate, kPayments[i]) << "period " << i;
        ASSERT_TRUE(f.average.has_value());
        EXPECT_NEAR(*f.average, averages[i], 1e-10) << "period " << i;
        EXPECT_NEAR(f.discount, kDiscounts[i], 1e-9) << "period " << i;
        EXPECT_EQ(f.realisedDays, i == 0 ? 9 : 0) << "period " << i;
        EXPECT_NEAR(f.presentValue, f.amount * f.discount, 1e-9);
        EXPECT_NEAR(f.amount, 10000.0 * *f.average, 1e-7);
        EXPECT_NEAR(fixedFlows[i].amount, -610000.0, 1e-9);
        for (const auto& o : f.observations) {
            days.push_back(o.date);
            EXPECT_EQ(o.source, o.date < kAsOf ? PriceSource::Fixing : PriceSource::Curve);
            if (changes) {
                EXPECT_EQ(o.contractMonth, expectedMonth(*changes, o.date))
                    << QuantLib::io::iso_date(o.date);
            } else {
                EXPECT_TRUE(o.contractMonth.empty());
            }
        }
    }
    EXPECT_EQ(days, expectedDays);

    const auto& p1 = floatFlows[0];
    EXPECT_EQ(p1.start, D(2026, 11, 1));
    EXPECT_EQ(p1.end, D(2026, 11, 30));
    ASSERT_TRUE(p1.realisedAverage.has_value());
    ASSERT_TRUE(p1.expectedAverage.has_value());
    EXPECT_NEAR((9 * *p1.realisedAverage + 11 * *p1.expectedAverage) / 20.0, *p1.average, 1e-12);
    EXPECT_FALSE(floatFlows[1].realisedAverage.has_value());
    EXPECT_NEAR(r.npv, r.legs[0].npv + r.legs[1].npv, 1e-9);
}

} // namespace

// ---------------------------------------------------------------------------
// a. periods
// ---------------------------------------------------------------------------

TEST(CommodityPeriods, MonthlyCutFirstPeriod) {
    const auto p = GeneratePeriods(D(2026, 11, 15), D(2027, 3, 31), QuantLib::Monthly, "periods");
    ASSERT_EQ(p.size(), 5u);
    EXPECT_EQ(p[0].start, D(2026, 11, 15));
    EXPECT_EQ(p[0].end, D(2026, 11, 30));
    EXPECT_EQ(p[1].start, D(2026, 12, 1));
    EXPECT_EQ(p[1].end, D(2026, 12, 31));
    EXPECT_EQ(p[2].start, D(2027, 1, 1));
    EXPECT_EQ(p[3].end, D(2027, 2, 28));
    EXPECT_EQ(p[4].start, D(2027, 3, 1));
    EXPECT_EQ(p[4].end, D(2027, 3, 31));
}

TEST(CommodityPeriods, QuarterlyCutBothEnds) {
    const auto p = GeneratePeriods(D(2026, 11, 15), D(2027, 8, 10), QuantLib::Quarterly, "periods");
    ASSERT_EQ(p.size(), 4u);
    EXPECT_EQ(p[0].start, D(2026, 11, 15));
    EXPECT_EQ(p[0].end, D(2026, 12, 31));
    EXPECT_EQ(p[1].start, D(2027, 1, 1));
    EXPECT_EQ(p[1].end, D(2027, 3, 31));
    EXPECT_EQ(p[2].start, D(2027, 4, 1));
    EXPECT_EQ(p[2].end, D(2027, 6, 30));
    EXPECT_EQ(p[3].start, D(2027, 7, 1));
    EXPECT_EQ(p[3].end, D(2027, 8, 10));
}

TEST(CommodityPeriods, AnnualWithLeapFebruary) {
    const auto p = GeneratePeriods(D(2026, 11, 1), D(2028, 2, 29), QuantLib::Annual, "periods");
    ASSERT_EQ(p.size(), 3u);
    EXPECT_EQ(p[0].start, D(2026, 11, 1));
    EXPECT_EQ(p[0].end, D(2026, 12, 31));
    EXPECT_EQ(p[1].start, D(2027, 1, 1));
    EXPECT_EQ(p[1].end, D(2027, 12, 31));
    EXPECT_EQ(p[2].start, D(2028, 1, 1));
    EXPECT_EQ(p[2].end, D(2028, 2, 29));
    const auto m = GeneratePeriods(D(2028, 2, 1), D(2028, 3, 31), QuantLib::Monthly, "periods");
    ASSERT_EQ(m.size(), 2u);
    EXPECT_EQ(m[0].end, D(2028, 2, 29));
}

TEST(CommodityPeriods, SemiannualOnceAndSingleDay) {
    const auto s = GeneratePeriods(D(2026, 5, 10), D(2027, 9, 30), QuantLib::Semiannual, "periods");
    ASSERT_EQ(s.size(), 4u);
    EXPECT_EQ(s[0].end, D(2026, 6, 30));
    EXPECT_EQ(s[1].start, D(2026, 7, 1));
    EXPECT_EQ(s[1].end, D(2026, 12, 31));
    EXPECT_EQ(s[2].end, D(2027, 6, 30));
    EXPECT_EQ(s[3].start, D(2027, 7, 1));
    EXPECT_EQ(s[3].end, D(2027, 9, 30));
    const auto o = GeneratePeriods(D(2026, 11, 15), D(2027, 8, 10), QuantLib::Once, "periods");
    ASSERT_EQ(o.size(), 1u);
    EXPECT_EQ(o[0].start, D(2026, 11, 15));
    EXPECT_EQ(o[0].end, D(2027, 8, 10));
    const auto one =
        GeneratePeriods(D(2026, 12, 31), D(2026, 12, 31), QuantLib::Monthly, "periods");
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].start, one[0].end);
}

TEST(CommodityPeriods, GenerationErrors) {
    expectInvalid(
        [] { GeneratePeriods(D(2026, 11, 1), D(2027, 1, 1), QuantLib::Weekly, "periods"); },
        "frequency");
    expectInvalid(
        [] { GeneratePeriods(D(2026, 11, 1), D(2027, 1, 1), QuantLib::Daily, "periods"); },
        "frequency");
    expectInvalid(
        [] { GeneratePeriods(D(2027, 1, 2), D(2027, 1, 1), QuantLib::Monthly, "periods"); },
        "is after");
}

TEST(CommodityPeriods, ExplicitValidation) {
    ValidatePeriods(
        {{D(2026, 11, 1), D(2026, 11, 30)}, {D(2027, 1, 1), D(2027, 1, 31)}}, "periods");
    ValidatePeriods({{D(2026, 11, 1), D(2026, 11, 1)}}, "periods");
    expectInvalid([] { ValidatePeriods({}, "periods"); }, "at least one");
    expectInvalid(
        [] { ValidatePeriods({{D(2026, 11, 2), D(2026, 11, 1)}}, "periods"); }, "periods[0]");
    expectInvalid(
        [] {
            ValidatePeriods(
                {{D(2026, 11, 1), D(2026, 11, 30)}, {D(2026, 11, 30), D(2026, 12, 31)}}, "periods");
        },
        "periods[1]");
    expectInvalid(
        [] {
            ValidatePeriods(
                {{D(2026, 12, 1), D(2026, 12, 31)}, {D(2026, 11, 1), D(2026, 11, 30)}}, "periods");
        },
        "ascending");
}

// ---------------------------------------------------------------------------
// b. pricing days
// ---------------------------------------------------------------------------

TEST(CommodityPricingDays, OwnHolidaysRemoveDays) {
    const PricingCalendar plain(QuantLib::UnitedStates(QuantLib::UnitedStates::NYSE), {});
    const PricingCalendar own(
        QuantLib::UnitedStates(QuantLib::UnitedStates::NYSE), {D(2026, 11, 27), D(2026, 11, 28)});
    const auto a = plain.pricingDays(D(2026, 11, 1), D(2026, 11, 30));
    const auto b = own.pricingDays(D(2026, 11, 1), D(2026, 11, 30));
    EXPECT_EQ(a.size(), 20u); // Thanksgiving 26 Nov closed
    EXPECT_EQ(b.size(), 19u); // + 27 Nov (28 Nov is a Saturday)
    EXPECT_FALSE(own.isPricingDay(D(2026, 11, 27)));
    EXPECT_TRUE(plain.isPricingDay(D(2026, 11, 27)));
    // Own holidays are the object's own: the named calendar is unchanged.
    EXPECT_TRUE(
        QuantLib::UnitedStates(QuantLib::UnitedStates::NYSE).isBusinessDay(D(2026, 11, 27)));
    EXPECT_EQ(own.pricingDaysBefore(D(2026, 11, 30), 1), D(2026, 11, 25));
    EXPECT_EQ(own.pricingDaysBefore(D(2026, 11, 29), 0), D(2026, 11, 29));
}

TEST(CommodityPricingDays, EmptyFloatingPeriodRefused) {
    const PricingCalendar cal(QuantLib::WeekendsOnly(), {});
    EXPECT_EQ(PeriodPricingDays(cal, {D(2026, 11, 2), D(2026, 11, 6)}, "x").size(), 5u);
    expectInvalid(
        [&] { PeriodPricingDays(cal, {D(2026, 11, 7), D(2026, 11, 8)}, "leg1.periods[3]"); },
        "leg1.periods[3] (2026-11-07 to 2026-11-08) has no pricing day");
    expectInvalid([] { PricingCalendar(QuantLib::Calendar(), {}); }, "calendar");
}

// ---------------------------------------------------------------------------
// c. contract selection
// ---------------------------------------------------------------------------

TEST(CommodityContract, RollZeroReadsExpiringMonthOnLastTradeDate) {
    const auto c = referenceContract(PricingCalendar(QuantLib::WeekendsOnly(), {}));
    // 2026-12 last trades Friday 20 Nov.
    EXPECT_EQ(monthRead(*c, D(2026, 11, 19), 1, 0), "2026-12");
    EXPECT_EQ(monthRead(*c, D(2026, 11, 20), 1, 0), "2026-12");
    EXPECT_EQ(monthRead(*c, D(2026, 11, 21), 1, 0), "2027-01");
    EXPECT_EQ(monthRead(*c, D(2026, 11, 23), 1, 0), "2027-01");
    // nearby 2 = one month further.
    EXPECT_EQ(monthRead(*c, D(2026, 11, 20), 2, 0), "2027-01");
    EXPECT_EQ(monthRead(*c, D(2026, 11, 23), 2, 0), "2027-02");
}

TEST(CommodityContract, RollTwoSwitchesTwoPricingDaysEarlier) {
    const auto c = referenceContract(PricingCalendar(QuantLib::WeekendsOnly(), {}));
    // Roll date = Wednesday 18 Nov: 18 Nov still on 2026-12, 19 and 20 Nov on 2027-01.
    EXPECT_EQ(c->rollDates(2)[1], D(2026, 11, 18));
    EXPECT_EQ(monthRead(*c, D(2026, 11, 18), 1, 2), "2026-12");
    EXPECT_EQ(monthRead(*c, D(2026, 11, 19), 1, 2), "2027-01");
    EXPECT_EQ(monthRead(*c, D(2026, 11, 20), 1, 2), "2027-01");
    // Over a weekend: 2027-03 last trades Monday 22 Feb 2027 -> roll date Thursday 18 Feb.
    EXPECT_EQ(c->rollDates(2)[4], D(2027, 2, 18));
    EXPECT_EQ(monthRead(*c, D(2027, 2, 18), 1, 2), "2027-03");
    EXPECT_EQ(monthRead(*c, D(2027, 2, 19), 1, 2), "2027-04");
}

TEST(CommodityContract, OwnHolidayMovesTheRollDate) {
    // 19 Nov an own holiday: two pricing days before 20 Nov is 17 Nov.
    const auto c = referenceContract(PricingCalendar(QuantLib::WeekendsOnly(), {D(2026, 11, 19)}));
    EXPECT_EQ(c->rollDates(2)[1], D(2026, 11, 17));
    EXPECT_EQ(monthRead(*c, D(2026, 11, 18), 1, 2), "2027-01");
}

TEST(CommodityContract, Errors) {
    const PricingCalendar cal(QuantLib::WeekendsOnly(), {});
    const auto c = referenceContract(cal);
    expectInvalid([&] { monthRead(*c, D(2027, 12, 21), 1, 0); }, "2027-12-21");
    expectInvalid([&] { monthRead(*c, D(2027, 12, 20), 2, 0); }, "2027-12-20");
    expectInvalid([&] { c->selectMonth(D(2026, 11, 2), 0, c->rollDates(0)); }, "nearby");
    expectInvalid([] { ValidateNearbyAndRoll(0, 0, "leg2.price"); }, "leg2.price.nearby");
    expectInvalid(
        [] { ValidateNearbyAndRoll(1, -1, "leg2.price"); }, "leg2.price.roll_days_before_expiry");
    ValidateNearbyAndRoll(1, 0, "leg2.price");
    auto months = referenceMonths();
    expectInvalid([&] { FuturesContract("X", cal, {}, {}); }, "at least one month");
    auto bad = months;
    std::swap(bad[1], bad[2]);
    expectInvalid([&] { FuturesContract("X", cal, bad, {}); }, "ascending");
    bad = months;
    bad[2].lastTradeDate = bad[1].lastTradeDate;
    expectInvalid([&] { FuturesContract("X", cal, bad, {}); }, "last_trade_date");
    bad = months;
    bad[0].month = "2026-13";
    expectInvalid([&] { FuturesContract("X", cal, bad, {}); }, "YYYY-MM");
    bad = months;
    bad[1].price = std::nan("");
    expectInvalid([&] { FuturesContract("X", cal, bad, {}); }, "finite");
    auto fixings = referenceContractFixings();
    fixings.push_back(fixings.front());
    expectInvalid([&] { FuturesContract("X", cal, months, fixings); }, "second fixing");
}

TEST(CommodityObservation, FixingsAndTodaysPrices) {
    const auto c = referenceContract(PricingCalendar(QuantLib::WeekendsOnly(), {}));
    // Before the valuation date: the fixing.
    auto o = ObserveFutures(*c, 1, D(2026, 11, 12), kAsOf);
    EXPECT_EQ(o.source, PriceSource::Fixing);
    EXPECT_EQ(o.price, 60.0);
    EXPECT_EQ(o.contractMonth, "2026-12");
    // On the valuation date without a fixing: today's price; with one: the fixing.
    o = ObserveFutures(*c, 1, kAsOf, kAsOf);
    EXPECT_EQ(o.source, PriceSource::Curve);
    EXPECT_EQ(o.price, 60.0);
    o = ObserveFutures(*c, 1, D(2026, 11, 12), D(2026, 11, 12));
    EXPECT_EQ(o.source, PriceSource::Fixing);
    // Missing fixing before the valuation date: named day and month.
    expectInvalid([&] { ObserveFutures(*c, 1, D(2026, 11, 16), D(2026, 11, 17)); }, "2026-11-16");
    expectInvalid([&] { ObserveFutures(*c, 2, D(2026, 11, 16), D(2026, 11, 17)); }, "2027-01");
    // Month without a price read after the valuation date.
    expectInvalid(
        [&] { ObserveFutures(*c, 0, D(2026, 11, 16), kAsOf); }, "month 2026-11 has no price");
    const auto s = referenceSpot(PricingCalendar(QuantLib::WeekendsOnly(), {}));
    expectInvalid([&] { ObserveSpot(*s, D(2026, 11, 16), D(2026, 11, 17)); }, "SYN_SPOT");
    o = ObserveSpot(*s, D(2026, 11, 12), kAsOf).observation;
    EXPECT_EQ(o.source, PriceSource::Fixing);
    EXPECT_NEAR(o.price, 59.8, 1e-12);
    EXPECT_TRUE(o.contractMonth.empty());
}

// ---------------------------------------------------------------------------
// d. spot price
// ---------------------------------------------------------------------------

TEST(CommoditySpot, Interpolations) {
    const PricingCalendar cal(QuantLib::WeekendsOnly(), {});
    const std::vector<SpotPoint> pts = {{D(2027, 1, 1), 10.0}, {D(2027, 1, 11), 20.0}};
    const SpotPrice lin("S", cal, SpotInterpolation::Linear, false, pts, {});
    const SpotPrice bwd("S", cal, SpotInterpolation::BackwardFlat, false, pts, {});
    const SpotPrice fwd("S", cal, SpotInterpolation::ForwardFlat, false, pts, {});
    EXPECT_NEAR(lin.forward(D(2027, 1, 4)), 13.0, 1e-12);
    EXPECT_EQ(bwd.forward(D(2027, 1, 4)), 20.0);
    EXPECT_EQ(fwd.forward(D(2027, 1, 4)), 10.0);
    for (const SpotPrice* s : {&lin, &bwd, &fwd}) {
        EXPECT_EQ(s->forward(D(2027, 1, 1)), 10.0);
        EXPECT_EQ(s->forward(D(2027, 1, 11)), 20.0);
        expectInvalid([&] { s->forward(D(2026, 12, 31)); }, "extrapolate_flat");
        expectInvalid([&] { s->forward(D(2027, 1, 12)); }, "extrapolate_flat");
    }
    const auto w = lin.weights(D(2027, 1, 4));
    ASSERT_EQ(w.size(), 2u);
    EXPECT_NEAR(w[0].weight, 0.7, 1e-15);
    EXPECT_NEAR(w[1].weight, 0.3, 1e-15);
    const SpotPrice flat("S", cal, SpotInterpolation::Linear, true, pts, {});
    EXPECT_EQ(flat.forward(D(2026, 12, 1)), 10.0);
    EXPECT_EQ(flat.forward(D(2028, 1, 1)), 20.0);
}

TEST(CommoditySpot, Errors) {
    const PricingCalendar cal(QuantLib::WeekendsOnly(), {});
    const std::vector<SpotPoint> pts = {{D(2027, 1, 1), 10.0}, {D(2027, 1, 11), 20.0}};
    expectInvalid([&] { SpotPrice("S", cal, SpotInterpolation::Linear, false, {}, {}); }, "point");
    expectInvalid(
        [&] { SpotPrice("S", cal, SpotInterpolation::Linear, false, {pts[1], pts[0]}, {}); },
        "ascending");
    expectInvalid(
        [&] {
            SpotPrice(
                "S", cal, SpotInterpolation::Linear, false, pts,
                {{D(2026, 1, 2), 1.0}, {D(2026, 1, 2), 2.0}});
        },
        "second fixing");
}

// ---------------------------------------------------------------------------
// e. reference values (synthetic data)
// ---------------------------------------------------------------------------

TEST(CommoditySwapReference, FuturesRollZero) {
    ReferenceCalendarScope scope;
    const auto r = PriceCommoditySwap(
        fixedPayLeg(), futuresLeg(referenceContract(referencePricingCalendar()), 0), kAsOf);
    checkReferenceSwap(r, 36038.838994, 61.3066474593, kAvgRoll0, &kChangesRoll0);
    const auto& p1 = r.legs[1].flows[0];
    EXPECT_EQ(p1.pricingDays, 20);
    EXPECT_EQ(p1.realisedDays, 9);
    EXPECT_NEAR(*p1.average, 60.025, 1e-10);
    EXPECT_EQ(p1.paymentDate, D(2026, 12, 7));
    EXPECT_NEAR(p1.discount, 0.9973733188, 1e-9);
}

TEST(CommoditySwapReference, FuturesRollTwo) {
    ReferenceCalendarScope scope;
    const auto r = PriceCommoditySwap(
        fixedPayLeg(), futuresLeg(referenceContract(referencePricingCalendar()), 2), kAsOf);
    checkReferenceSwap(r, 38048.121125, 61.3237440495, kAvgRoll2, &kChangesRoll2);
    EXPECT_NEAR(*r.legs[1].flows[0].average, 60.065, 1e-10);
}

TEST(CommoditySwapReference, SpotPrice) {
    ReferenceCalendarScope scope;
    const auto r = PriceCommoditySwap(
        fixedPayLeg(), spotLeg(referenceSpot(referencePricingCalendar())), kAsOf);
    checkReferenceSwap(r, -15486.170077, 60.8682311958, kAvgSpot, nullptr);
    EXPECT_NEAR(*r.legs[1].flows[0].average, 59.48375, 1e-10);
}

TEST(CommoditySwapReference, OwnHolidaysGiveTheSamePricingDays) {
    // Same exchange days from a weekdays-only calendar plus own holidays.
    const auto cal = ownHolidaysPricingCalendar();
    const auto r0 = PriceCommoditySwap(fixedPayLeg(), futuresLeg(referenceContract(cal), 0), kAsOf);
    checkReferenceSwap(r0, 36038.838994, 61.3066474593, kAvgRoll0, &kChangesRoll0);
    const auto r2 = PriceCommoditySwap(fixedPayLeg(), futuresLeg(referenceContract(cal), 2), kAsOf);
    checkReferenceSwap(r2, 38048.121125, 61.3237440495, kAvgRoll2, &kChangesRoll2);
    const auto rs = PriceCommoditySwap(fixedPayLeg(), spotLeg(referenceSpot(cal)), kAsOf);
    checkReferenceSwap(rs, -15486.170077, 60.8682311958, kAvgSpot, nullptr);
}

// ---------------------------------------------------------------------------
// f. properties
// ---------------------------------------------------------------------------

TEST(CommoditySwapProperties, PayerIsMinusReceiver) {
    const auto cal = ownHolidaysPricingCalendar();
    const auto contract = referenceContract(cal);
    auto fixedRecv = fixedPayLeg();
    fixedRecv.payReceive = PayReceive::Receive;
    const auto a = PriceCommoditySwap(fixedPayLeg(), futuresLeg(contract, 0), kAsOf);
    const auto b = PriceCommoditySwap(fixedRecv, futuresLeg(contract, 0, PayReceive::Pay), kAsOf);
    EXPECT_NEAR(a.npv, -b.npv, 1e-9);
    EXPECT_NEAR(a.legs[0].npv, -b.legs[0].npv, 1e-9);
    EXPECT_NEAR(*a.fairFixedPrice, *b.fairFixedPrice, 1e-12);
    ASSERT_EQ(a.deltas.size(), b.deltas.size());
    for (std::size_t i = 0; i < a.deltas.size(); ++i) {
        EXPECT_EQ(a.deltas[i].month, b.deltas[i].month);
        EXPECT_NEAR(a.deltas[i].delta, -b.deltas[i].delta, 1e-9);
    }
}

TEST(CommoditySwapProperties, ZeroNpvAtFairFixedPrice) {
    const auto cal = ownHolidaysPricingCalendar();
    for (int roll : {0, 2}) {
        const auto contract = referenceContract(cal);
        const auto r = PriceCommoditySwap(fixedPayLeg(), futuresLeg(contract, roll), kAsOf);
        const auto z =
            PriceCommoditySwap(fixedPayLeg(*r.fairFixedPrice), futuresLeg(contract, roll), kAsOf);
        EXPECT_NEAR(z.npv, 0.0, 1e-8);
        // Fixed leg second: same fair price.
        auto fixedRecv = fixedPayLeg();
        fixedRecv.payReceive = PayReceive::Receive;
        const auto r2 =
            PriceCommoditySwap(futuresLeg(contract, roll, PayReceive::Pay), fixedRecv, kAsOf);
        EXPECT_NEAR(*r2.fairFixedPrice, *r.fairFixedPrice, 1e-10);
        EXPECT_FALSE(r2.fairSpread.has_value());
    }
}

TEST(CommoditySwapProperties, ZeroNpvAtFairSpread) {
    const auto cal = ownHolidaysPricingCalendar();
    const auto contract = referenceContract(cal);
    const auto spot = referenceSpot(cal);
    const auto r = PriceCommoditySwap(
        futuresLeg(contract, 0, PayReceive::Receive), spotLeg(spot, PayReceive::Pay, 0.1), kAsOf);
    EXPECT_FALSE(r.fairFixedPrice.has_value());
    ASSERT_TRUE(r.fairSpread.has_value());
    const auto z = PriceCommoditySwap(
        futuresLeg(contract, 0, PayReceive::Receive), spotLeg(spot, PayReceive::Pay, *r.fairSpread),
        kAsOf);
    EXPECT_NEAR(z.npv, 0.0, 1e-8);
    for (const auto& f : z.legs[1].flows) {
        EXPECT_EQ(*f.spread, *r.fairSpread);
    }
    // Fixed against fixed: neither fair value.
    auto fixedRecv = fixedPayLeg(60.0);
    fixedRecv.payReceive = PayReceive::Receive;
    const auto ff = PriceCommoditySwap(fixedPayLeg(), fixedRecv, kAsOf);
    EXPECT_FALSE(ff.fairFixedPrice.has_value());
    EXPECT_FALSE(ff.fairSpread.has_value());
    EXPECT_TRUE(ff.deltas.empty());
}

TEST(CommoditySwapProperties, FullyRealisedPeriod) {
    // Valuation 2026-12-01: every November pricing day is fixed.
    const auto cal = ownHolidaysPricingCalendar();
    auto fixings = referenceSpotFixings();
    double v = 60.0;
    for (const auto& d : cal.pricingDays(D(2026, 11, 13), D(2026, 11, 30))) {
        fixings.push_back({d, v});
        v += 0.05;
    }
    const auto spot = std::make_shared<SpotPrice>(
        "SYN_SPOT", cal, SpotInterpolation::Linear, true, referenceSpotPoints(), fixings);
    const auto r = PriceCommoditySwap(fixedPayLeg(), spotLeg(spot), D(2026, 12, 1));
    const auto& p1 = r.legs[1].flows[0];
    EXPECT_EQ(p1.realisedDays, p1.pricingDays);
    EXPECT_EQ(p1.realisedDays, 20);
    ASSERT_TRUE(p1.realisedAverage.has_value());
    EXPECT_NEAR(*p1.average, *p1.realisedAverage, 1e-12);
    EXPECT_FALSE(p1.expectedAverage.has_value());
    // 1 Dec is the valuation date with no fixing: read from the curve.
    const auto& p2 = r.legs[1].flows[1];
    EXPECT_EQ(p2.realisedDays, 0);
    EXPECT_FALSE(p2.realisedAverage.has_value());
    // Fixings read no market price, so they carry no delta.
    for (const auto& o : p1.observations) {
        const auto read = ObserveSpot(*spot, o.date, D(2026, 12, 1));
        EXPECT_EQ(read.observation.source, PriceSource::Fixing);
        EXPECT_TRUE(read.weights.empty());
    }
}

TEST(CommoditySwapProperties, DeltasMatchFiniteDifferences) {
    const auto cal = ownHolidaysPricingCalendar();
    const double h = 0.01;
    // Futures: bump each month's price.
    for (int roll : {0, 2}) {
        const auto base =
            PriceCommoditySwap(fixedPayLeg(), futuresLeg(referenceContract(cal), roll), kAsOf);
        ASSERT_EQ(base.deltas.size(), 13u); // 2026-12 .. 2027-12
        for (const auto& d : base.deltas) {
            EXPECT_EQ(d.kind, CommodityPriceDelta::Kind::Futures);
            EXPECT_EQ(d.contractId, "SYN_CL");
            auto bumped = [&](double shift) {
                auto months = referenceMonths();
                for (auto& m : months)
                    if (m.month == d.month) *m.price += shift;
                auto c = std::make_shared<FuturesContract>(
                    "SYN_CL", cal, months, referenceContractFixings());
                return PriceCommoditySwap(fixedPayLeg(), futuresLeg(c, roll), kAsOf).npv;
            };
            const double fd = (bumped(h) - bumped(-h)) / (2 * h);
            EXPECT_NEAR(d.delta, fd, 1e-8 * std::abs(fd)) << d.month;
        }
    }
    // Spot: bump each point.
    const auto base = PriceCommoditySwap(fixedPayLeg(), spotLeg(referenceSpot(cal)), kAsOf);
    ASSERT_FALSE(base.deltas.empty());
    for (const auto& d : base.deltas) {
        EXPECT_EQ(d.kind, CommodityPriceDelta::Kind::Spot);
        auto bumped = [&](double shift) {
            auto pts = referenceSpotPoints();
            for (auto& p : pts)
                if (p.date == d.date) p.price += shift;
            auto s = std::make_shared<SpotPrice>(
                "SYN_SPOT", cal, SpotInterpolation::Linear, true, pts, referenceSpotFixings());
            return PriceCommoditySwap(fixedPayLeg(), spotLeg(s), kAsOf).npv;
        };
        const double fd = (bumped(h) - bumped(-h)) / (2 * h);
        EXPECT_NEAR(d.delta, fd, 1e-8 * std::abs(fd)) << QuantLib::io::iso_date(d.date);
    }
    // Points no pricing day reads carry no delta.
    double sum = 0.0;
    for (const auto& d : base.deltas)
        sum += d.delta;
    for (const auto& d : base.deltas)
        EXPECT_LE(d.date, D(2027, 11, 15));
    EXPECT_GT(sum, 0.0);
}

TEST(CommoditySwapProperties, PerPricingDayMatchesScaledPerPeriod) {
    const auto cal = ownHolidaysPricingCalendar();
    const auto contract = referenceContract(cal);
    auto fixedDaily = fixedPayLeg();
    fixedDaily.quantity.basis = QuantityBasis::PerPricingDay;
    fixedDaily.quantity.value = 500.0;
    auto floatDaily = futuresLeg(contract, 0);
    floatDaily.quantity.basis = QuantityBasis::PerPricingDay;
    floatDaily.quantity.value = 500.0;
    const auto daily = PriceCommoditySwap(fixedDaily, floatDaily, kAsOf);

    std::vector<double> scaled;
    for (int n : kPricingDays)
        scaled.push_back(500.0 * n);
    auto fixedScaled = fixedPayLeg();
    fixedScaled.quantity.value.reset();
    fixedScaled.quantity.values = scaled;
    auto floatScaled = futuresLeg(contract, 0);
    floatScaled.quantity.value.reset();
    floatScaled.quantity.values = scaled;
    const auto period = PriceCommoditySwap(fixedScaled, floatScaled, kAsOf);

    EXPECT_NEAR(daily.npv, period.npv, 1e-8);
    EXPECT_NEAR(*daily.fairFixedPrice, *period.fairFixedPrice, 1e-10);
    for (std::size_t i = 0; i < 12; ++i) {
        EXPECT_EQ(daily.legs[0].flows[i].quantity, scaled[i]);
        EXPECT_EQ(daily.legs[0].flows[i].pricingDays, kPricingDays[i]);
        EXPECT_EQ(daily.legs[1].flows[i].quantity, scaled[i]);
    }
    // A fixed leg per pricing day needs a floating period with the same dates.
    auto fixedOther = fixedDaily;
    fixedOther.periods =
        GeneratePeriods(D(2026, 11, 2), D(2027, 10, 31), QuantLib::Monthly, "periods");
    expectInvalid([&] { PriceCommoditySwap(fixedOther, floatDaily, kAsOf); }, "same dates");
    auto fixedRecv = fixedPayLeg();
    fixedRecv.payReceive = PayReceive::Receive;
    expectInvalid([&] { PriceCommoditySwap(fixedDaily, fixedRecv, kAsOf); }, "floating leg");
}

TEST(CommoditySwapProperties, PaymentOnValuationDateIsLeftOut) {
    // Period 1 pays on 2026-12-07: valued on that date it is already paid,
    // as the engine's rate swaps treat a flow on the valuation date.
    auto fixedRecv = fixedPayLeg(60.0);
    fixedRecv.payReceive = PayReceive::Receive;
    const auto on = PriceCommoditySwap(fixedPayLeg(), fixedRecv, D(2026, 12, 7));
    ASSERT_EQ(on.legs[0].flows.size(), 11u);
    EXPECT_EQ(on.legs[0].flows[0].paymentDate, D(2027, 1, 8));
    const auto before = PriceCommoditySwap(fixedPayLeg(), fixedRecv, D(2026, 12, 4));
    ASSERT_EQ(before.legs[0].flows.size(), 12u);
    EXPECT_EQ(before.legs[0].flows[0].paymentDate, D(2026, 12, 7));
    // Paid flows read no prices: a futures leg valued after the first
    // payment needs no November or early December fixings.
    const auto cal = ownHolidaysPricingCalendar();
    auto months = referenceMonths();
    const auto c =
        std::make_shared<FuturesContract>("SYN_CL", cal, months, std::vector<ContractFixing>{});
    expectInvalid(
        [&] { PriceCommoditySwap(fixedPayLeg(), futuresLeg(c, 0), D(2026, 12, 7)); }, "2026-12-01");
}

TEST(CommoditySwapProperties, SwapValidationErrors) {
    const auto cal = ownHolidaysPricingCalendar();
    const auto contract = referenceContract(cal);
    const auto fl = futuresLeg(contract, 0);
    expectInvalid(
        [&] { PriceCommoditySwap(fixedPayLeg(), futuresLeg(contract, 0, PayReceive::Pay), kAsOf); },
        "opposite");
    auto leg = fixedPayLeg();
    leg.quantity.values = {1.0};
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "exactly one");
    leg = fixedPayLeg();
    leg.quantity.value.reset();
    leg.quantity.values = {1.0, 2.0};
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "2 entries for 12 periods");
    leg = fixedPayLeg();
    leg.quantity.value = -1.0;
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "quantity");
    leg = fixedPayLeg();
    leg.price = FixedPrice{std::nullopt, {61.0, 62.0}};
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "2 entries for 12 periods");
    leg = fixedPayLeg();
    leg.price = FixedPrice{};
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "exactly one");
    leg = fixedPayLeg();
    leg.paymentLag = -1;
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "payment_lag");
    leg = fixedPayLeg();
    leg.discountCurve = QuantLib::Handle<QuantLib::YieldTermStructure>();
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "leg1.discounting_curve");
    leg = fixedPayLeg();
    leg.paymentCalendar = QuantLib::Calendar();
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "payment_calendar");
    leg = fixedPayLeg();
    leg.periods = {};
    expectInvalid([&] { PriceCommoditySwap(leg, fl, kAsOf); }, "leg1");
    auto badFloat = futuresLeg(contract, 0);
    std::get<FuturesFloatingPrice>(badFloat.price).nearby = 0;
    expectInvalid([&] { PriceCommoditySwap(fixedPayLeg(), badFloat, kAsOf); }, "nearby");
    badFloat = futuresLeg(nullptr, 0);
    expectInvalid([&] { PriceCommoditySwap(fixedPayLeg(), badFloat, kAsOf); }, "contract");
    badFloat = spotLeg(nullptr);
    expectInvalid([&] { PriceCommoditySwap(fixedPayLeg(), badFloat, kAsOf); }, "spot");
    // A floating period without pricing days.
    badFloat = futuresLeg(contract, 0);
    badFloat.periods = {{D(2026, 11, 28), D(2026, 11, 29)}};
    leg = fixedPayLeg();
    leg.periods = badFloat.periods;
    expectInvalid([&] { PriceCommoditySwap(leg, badFloat, kAsOf); }, "no pricing day");
    // Month without a price read after the valuation date.
    auto months = referenceMonths();
    months[5].price.reset();
    const auto c =
        std::make_shared<FuturesContract>("SYN_CL", cal, months, referenceContractFixings());
    expectInvalid([&] { PriceCommoditySwap(fixedPayLeg(), futuresLeg(c, 0), kAsOf); }, "2027-04");
    // Spot beyond its last point without flat extrapolation.
    const auto s = std::make_shared<SpotPrice>(
        "SYN_SPOT", cal, SpotInterpolation::Linear, false, referenceSpotPoints(),
        referenceSpotFixings());
    expectInvalid(
        [&] { PriceCommoditySwap(fixedPayLeg(), spotLeg(s), kAsOf); }, "extrapolate_flat");
}

TEST(CommoditySwapProperties, PaidPeriodWithoutPricingDaysIsLeftOut) {
    // Period 0 is a weekend (no pricing day) paid in November; valued on
    // 2026-12-01 it is already paid, so its pricing days are never counted.
    const auto cal = ownHolidaysPricingCalendar();
    const auto contract = referenceContract(cal);
    const std::vector<CommodityPeriod> periods = {
        {D(2026, 11, 7), D(2026, 11, 8)}, {D(2026, 12, 1), D(2026, 12, 31)}};
    for (QuantityBasis basis : {QuantityBasis::PerPeriod, QuantityBasis::PerPricingDay}) {
        auto fixed = fixedPayLeg();
        fixed.periods = periods;
        fixed.quantity.basis = basis;
        auto floating = futuresLeg(contract, 0);
        floating.periods = periods;
        floating.quantity.basis = basis;
        const auto r = PriceCommoditySwap(fixed, floating, D(2026, 12, 1));
        ASSERT_EQ(r.legs[0].flows.size(), 1u);
        ASSERT_EQ(r.legs[1].flows.size(), 1u);
        EXPECT_EQ(r.legs[1].flows[0].start, D(2026, 12, 1));
        EXPECT_EQ(r.legs[1].flows[0].pricingDays, 22);
        EXPECT_EQ(r.legs[0].flows[0].pricingDays, basis == QuantityBasis::PerPricingDay ? 22 : 0);
        // Still to be paid, the same period is refused.
        expectInvalid(
            [&] { PriceCommoditySwap(fixed, floating, D(2026, 11, 2)); },
            "leg2.periods[0] (2026-11-07 to 2026-11-08) has no pricing day");
    }
}

TEST(CommoditySwapProperties, MessagesNameTheFieldOnce) {
    const auto cal = ownHolidaysPricingCalendar();
    const auto fl = futuresLeg(referenceContract(cal), 0);
    auto leg = fixedPayLeg();
    leg.quantity.value.reset();
    leg.quantity.values = std::vector<double>(3, 1.0);
    try {
        PriceCommoditySwap(leg, fl, kAsOf);
        ADD_FAILURE() << "expected an invalid argument";
    } catch (const QuantraInvalidArgument& e) {
        const std::string msg = e.what();
        EXPECT_EQ(msg, "leg1.quantity.values has 3 entries for 12 periods");
        EXPECT_EQ(
            QualifySwapMessage("swaps[2].commodity_swap", msg),
            "swaps[2].commodity_swap.leg1.quantity.values has 3 entries for 12 periods");
    }
    leg = fixedPayLeg();
    leg.periods = {{D(2026, 11, 1), D(2026, 11, 30)}, {D(2026, 11, 15), D(2026, 12, 31)}};
    leg.quantity.value = 1.0;
    expectInvalid(
        [&] { PriceCommoditySwap(leg, fl, kAsOf); },
        "leg1.periods.periods[1].start_date 2026-11-15 is not after the end of "
        "leg1.periods.periods[0]");
    auto badSpread = futuresLeg(referenceContract(cal), 0);
    std::get<FuturesFloatingPrice>(badSpread.price).spread = std::nan("");
    expectInvalid(
        [&] { PriceCommoditySwap(fixedPayLeg(), badSpread, kAsOf); },
        "leg2.price.spread must be finite");
    // Market data read while pricing names the contract and takes ": ".
    EXPECT_EQ(
        QualifySwapMessage("swaps[0].commodity_swap", "commodity contract 'X': no price"),
        "swaps[0].commodity_swap: commodity contract 'X': no price");
}

TEST(CommoditySwapProperties, OverflowIsRefused) {
    const auto cal = ownHolidaysPricingCalendar();
    auto fixed = fixedPayLeg(1e308);
    expectInvalid(
        [&] { PriceCommoditySwap(fixed, futuresLeg(referenceContract(cal), 0), kAsOf); },
        "is not finite (the numbers overflowed)");
}

// ---------------------------------------------------------------------------
// g. YYYY-MM months
// ---------------------------------------------------------------------------

TEST(CommodityMonths, IsYearMonth) {
    EXPECT_TRUE(IsYearMonth("2027-02"));
    EXPECT_TRUE(IsYearMonth("2027-12"));
    for (const std::string bad :
         {"2027-2", "2027/02", "2027-13", "2027-00", "Feb-27", "", "2027-02-01"}) {
        EXPECT_FALSE(IsYearMonth(bad)) << "'" << bad << "'";
    }
}
