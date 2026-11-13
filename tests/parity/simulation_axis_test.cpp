// Curves on the simulation's time axis (src/market/simulation_axis.h).
//
//   * on Actual/360, 30/360 (Bond Basis), Actual/Actual (ISDA) and
//     Actual/365 (Fixed) zero curves, the view read at the Actual/365 (Fixed)
//     time of a date equals the curve read at that date, for a ladder of
//     dates out to 10 years; a fraction of a day reads the curve's own time
//     interpolated between the two dates (exact on Actual/360);
//   * the identity rule: a curve on Actual/365 (Fixed) at the valuation date
//     is the same handle; another reference date gets a view, read by date,
//     1 before the curve's reference date;
//   * the view notifies its observers when the curve's quote moves or its
//     handle is relinked;
//   * a vol off the axis (day counter, reference date) is a 400 naming the
//     vol and the field.
#include "simulation_axis.h"

#include "error.h"

#include <ql/patterns/observable.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/time/daycounters/thirty360.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <string>
#include <vector>

using namespace QuantLib;

namespace {

const Date kAsOf(30, September, 2026);

Handle<YieldTermStructure> zeroCurve(const DayCounter& dc, const Date& ref = kAsOf) {
    std::vector<Date> dates{ref};
    std::vector<Rate> rates{0.031};
    const std::vector<std::pair<Period, Rate>> nodes{
        {Period(1, Months), 0.033},
        {Period(6, Months), 0.036},
        {Period(2, Years), 0.034},
        {Period(5, Years), 0.038},
        {Period(12, Years), 0.041}};
    for (const auto& [p, r] : nodes) {
        dates.push_back(ref + p);
        rates.push_back(r);
    }
    auto c = std::make_shared<ZeroCurve>(dates, rates, dc);
    c->enableExtrapolation();
    return Handle<YieldTermStructure>(c);
}

std::vector<Date> ladder() {
    std::vector<Date> out;
    for (int d = 0; d <= 3660; d += 7)
        out.push_back(kAsOf + d);
    // Month ends and February: where 30/360 and Actual/Actual move unevenly.
    for (int m = 0; m < 120; ++m)
        out.push_back(Date::endOfMonth(kAsOf + Period(m, Months)));
    return out;
}

class Flag : public Observer {
public:
    bool up = false;
    void update() override { up = true; }
};

} // namespace

TEST(SimulationAxis, ViewReadsTheCurveByDate) {
    const Actual365Fixed axis;
    const std::vector<DayCounter> dcs{
        Actual360(), Thirty360(Thirty360::BondBasis), ActualActual(ActualActual::ISDA),
        Actual365Fixed()};
    for (const auto& dc : dcs) {
        const auto inner = zeroCurve(dc);
        const auto view = quantra::OnSimulationAxis(inner, kAsOf);
        EXPECT_EQ(view->dayCounter(), axis) << dc.name();
        EXPECT_EQ(view->referenceDate(), kAsOf) << dc.name();
        for (const Date& d : ladder()) {
            const double expected = inner->discount(d, true);
            const double got = view->discount(axis.yearFraction(kAsOf, d), true);
            EXPECT_NEAR(got, expected, 1e-14 * expected) << dc.name() << " " << d;
            EXPECT_NEAR(view->discount(d, true), expected, 1e-14 * expected)
                << dc.name() << " " << d;
        }
    }
}

TEST(SimulationAxis, FractionOfADayInterpolatesTheCurvesOwnTime) {
    const auto inner = zeroCurve(Actual360());
    const auto view = quantra::OnSimulationAxis(inner, kAsOf);
    auto* v = dynamic_cast<quantra::SimulationAxisCurve*>(view.currentLink().get());
    ASSERT_NE(v, nullptr);
    for (double days : {0.25, 17.5, 400.0001, 1234.9}) {
        EXPECT_NEAR(v->innerTime(days / 365.0), days / 360.0, 1e-14) << days;
        const double expected = inner->discount(days / 360.0, true);
        EXPECT_NEAR(view->discount(days / 365.0, true), expected, 1e-14 * expected) << days;
    }
    // 30/360 across a month end: the curve's time does not move over the 31st.
    const auto thirty =
        quantra::OnSimulationAxis(zeroCurve(Thirty360(Thirty360::BondBasis)), kAsOf);
    auto* t = dynamic_cast<quantra::SimulationAxisCurve*>(thirty.currentLink().get());
    ASSERT_NE(t, nullptr);
    const Date may30(30, May, 2027);
    const double tMay30 = Actual365Fixed().yearFraction(kAsOf, may30);
    EXPECT_DOUBLE_EQ(t->innerTime(tMay30 + 0.5 / 365.0), t->innerTime(tMay30));
}

TEST(SimulationAxis, IdentityRule) {
    const auto onAxis = zeroCurve(Actual365Fixed());
    EXPECT_EQ(quantra::OnSimulationAxis(onAxis, kAsOf).currentLink(), onAxis.currentLink());
    EXPECT_TRUE(quantra::IsOnSimulationAxis(**onAxis, kAsOf));

    // Another convention of Actual/365 (Fixed) is another day counter.
    const auto noLeap = zeroCurve(Actual365Fixed(Actual365Fixed::NoLeap));
    EXPECT_NE(quantra::OnSimulationAxis(noLeap, kAsOf).currentLink(), noLeap.currentLink());

    // A reference date two days after the valuation date: read by date, 1
    // up to the curve's reference date.
    const Date ref = kAsOf + 2;
    const auto later = zeroCurve(Actual365Fixed(), ref);
    const auto view = quantra::OnSimulationAxis(later, kAsOf);
    ASSERT_NE(view.currentLink(), later.currentLink());
    EXPECT_EQ(view->referenceDate(), kAsOf);
    EXPECT_EQ(view->discount(0.0), 1.0);
    EXPECT_EQ(view->discount(1.0 / 365.0), 1.0);
    for (const Date& d : ladder()) {
        if (d < ref) continue;
        const double expected = later->discount(d, true);
        EXPECT_NEAR(view->discount(d, true), expected, 1e-14 * expected) << d;
    }
    EXPECT_TRUE(quantra::OnSimulationAxis(Handle<YieldTermStructure>(), kAsOf).empty());
}

TEST(SimulationAxis, ViewObservesTheCurve) {
    auto rate = std::make_shared<SimpleQuote>(0.03);
    RelinkableHandle<YieldTermStructure> inner(
        std::make_shared<FlatForward>(kAsOf, Handle<Quote>(rate), Actual360()));
    const auto view = quantra::OnSimulationAxis(inner, kAsOf);
    ASSERT_NE(view.currentLink(), inner.currentLink());
    Flag flag;
    flag.registerWith(view);

    const Date d = kAsOf + 400;
    const double before = view->discount(d);
    rate->setValue(0.04);
    EXPECT_TRUE(flag.up);
    EXPECT_NEAR(view->discount(d), inner->discount(d), 1e-15);
    EXPECT_LT(view->discount(d), before);

    flag.up = false;
    inner.linkTo(std::make_shared<FlatForward>(kAsOf, 0.05, Actual360()));
    EXPECT_TRUE(flag.up);
    EXPECT_NEAR(view->discount(d), std::exp(-0.05 * 400.0 / 360.0), 1e-15);
}

TEST(SimulationAxis, VolOffTheAxisIs400) {
    const BlackConstantVol onAxis(kAsOf, NullCalendar(), 0.2, Actual365Fixed());
    EXPECT_NO_THROW(quantra::RequireVolOnSimulationAxis(onAxis, kAsOf, "SPX_VOL"));

    const BlackConstantVol act360(kAsOf, NullCalendar(), 0.2, Actual360());
    try {
        quantra::RequireVolOnSimulationAxis(act360, kAsOf, "SPX_VOL");
        FAIL() << "expected a 400";
    } catch (const QuantraInvalidArgument& e) {
        const std::string what = e.what();
        EXPECT_NE(
            what.find("volatility 'SPX_VOL': base.day_counter must be Actual365Fixed"),
            std::string::npos)
            << what;
        EXPECT_NE(what.find("Actual/360"), std::string::npos) << what;
    }

    const BlackConstantVol later(kAsOf + 1, NullCalendar(), 0.2, Actual365Fixed());
    try {
        quantra::RequireVolOnSimulationAxis(later, kAsOf, "SPX_VOL");
        FAIL() << "expected a 400";
    } catch (const QuantraInvalidArgument& e) {
        const std::string what = e.what();
        EXPECT_NE(
            what.find(
                "volatility 'SPX_VOL': base.reference_date must be the as_of_date "
                "2026-09-30"),
            std::string::npos)
            << what;
        EXPECT_NE(what.find("got 2026-10-01"), std::string::npos) << what;
    }
}
