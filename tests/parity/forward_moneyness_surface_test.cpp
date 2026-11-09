// Time interpolation at fixed forward moneyness (SmileSliceVolSurface with a
// SurfaceForward; SviTimeInterpolation LinearTotalVarianceForwardMoneyness,
// SurfaceInterpolator2D CubicStrikeLinearTimeForwardMoneyness).
//
// QuantLib's LocalVolSurface differentiates the total variance in time along
// the forward. Two SVI slices ordered at fixed moneyness k = ln(K / F) by a
// small margin, with a strong smile curvature and a large forward move
// between them, read at fixed strike show a decreasing variance along that
// forward line just before the second expiry (the second-order term
// x^2 / 2 d2w/dk2 of the forward move x exceeds the margin) and QuantLib
// refuses; read at fixed moneyness the derivative along the forward is the
// margin divided by the interval, whatever the strike, and nothing is
// refused, also when the process forward drifts slightly differently (a
// note's own dividend curve).

#include "basket_model.h"
#include "smile_slice_vol_surface.h"

#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;
using QuantLib::Real;
using QuantLib::Time;

struct DriftMarket {
    Date asOf{22, QuantLib::October, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    Date d1 = asOf + 183, d2 = asOf + 548;
    double spot = 95.0;
    double mu = 0.10;     // forward drift r - q of the calibration market
    double delta = 0.003; // calendar margin at fixed k
    SviSliceParams p1, p2;

    DriftMarket() {
        p1 = SviSliceParams{fwd(t1()), 0.02, 0.1, 0.1, -0.5, 0.0};
        p2 = p1;
        p2.forward = fwd(t2());
        p2.a += delta;
    }
    Time t1() const { return dc.yearFraction(asOf, d1); }
    Time t2() const { return dc.yearFraction(asOf, d2); }
    double fwd(Time t) const { return spot * std::exp(mu * t); }

    std::shared_ptr<SmileSliceVolSurface> surface(SviTimeReading r) const {
        auto s = makeSviSurface(
            asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, {d1, d2}, {p1, p2}, r,
            r == SviTimeReading::FixedForwardMoneyness ? std::optional<double>(spot)
                                                       : std::nullopt);
        s->enableExtrapolation();
        return s;
    }
    Handle<QuantLib::YieldTermStructure> flat(double rate) const {
        return Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(asOf, rate, dc));
    }
};

struct ScanResult {
    unsigned points = 0, refused = 0;
};

// QuantLib's Dupire local vol over t in (0, 1.6] x K in [60, 180] for a
// process with spot S, risk-free r and dividend q.
ScanResult dupireScan(
    const std::shared_ptr<SmileSliceVolSurface>& s,
    double spot,
    double r,
    double q,
    const DriftMarket& m) {
    QuantLib::LocalVolSurface lv(
        Handle<QuantLib::BlackVolTermStructure>(s), m.flat(r), m.flat(q),
        Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot)));
    lv.enableExtrapolation();
    ScanResult out;
    for (int i = 1; i <= 160; ++i) {
        const Time t = 0.01 * i;
        for (int j = 0; j <= 120; ++j) {
            const Real K = 60.0 * std::pow(3.0, j / 120.0);
            ++out.points;
            try {
                const Real v = lv.localVol(t, K, true);
                if (!(v >= 0.0) || !std::isfinite(v)) ++out.refused;
            } catch (const std::exception&) {
                ++out.refused;
            }
        }
    }
    return out;
}

} // namespace

TEST(ForwardMoneynessSurface, NodeForwardIsLnLinearAndExactAtTheNodes) {
    const NodeSurfaceForward f({0.5, 1.5}, {100.0, 110.0}, 95.0);
    EXPECT_EQ(f.forward(0.0), 95.0);
    EXPECT_EQ(f.forward(0.5), 100.0);
    EXPECT_EQ(f.forward(1.5), 110.0);
    const double l = std::log(100.0) + (std::log(110.0) - std::log(100.0)) * 0.5;
    EXPECT_NEAR(f.forward(1.0), std::exp(l), 1e-12);
    // Beyond the last node the last slope continues (a flat forward rate).
    EXPECT_NEAR(std::log(f.forward(2.5)), std::log(110.0) + std::log(110.0 / 100.0), 1e-12);
    // No spot: the first slope continued back; one node: constant.
    const NodeSurfaceForward g({0.5, 1.5}, {100.0, 110.0}, std::nullopt);
    EXPECT_NEAR(std::log(g.forward(0.0)), std::log(100.0) - 0.5 * std::log(1.1), 1e-12);
    const NodeSurfaceForward h({0.5}, {100.0}, std::nullopt);
    EXPECT_EQ(h.forward(0.1), 100.0);
    EXPECT_EQ(h.forward(3.0), 100.0);
    EXPECT_THROW(NodeSurfaceForward({0.5}, {-1.0}, std::nullopt), std::exception);
    EXPECT_THROW(NodeSurfaceForward({0.5, 0.5}, {1.0, 1.0}, std::nullopt), std::exception);
}

TEST(ForwardMoneynessSurface, PillarsAreTheSlicesAndBetweenThemTheForwardRatioShift) {
    DriftMarket m;
    const auto fs = m.surface(SviTimeReading::FixedStrike);
    const auto fm = m.surface(SviTimeReading::FixedForwardMoneyness);
    ASSERT_TRUE(fm->surfaceForward());
    EXPECT_FALSE(fs->surfaceForward());
    const SviSmileSlice s1(m.p1), s2(m.p2);
    for (Real K : {50.0, 80.0, 95.0, 100.0, 104.0, 130.0, 200.0}) {
        // At the pillars both readings are the slices, exactly.
        EXPECT_DOUBLE_EQ(fm->blackVariance(m.t1(), K, true), s1.totalVariance(K)) << K;
        EXPECT_DOUBLE_EQ(fm->blackVariance(m.t2(), K, true), s2.totalVariance(K)) << K;
        EXPECT_DOUBLE_EQ(fs->blackVariance(m.t1(), K, true), s1.totalVariance(K)) << K;
        // Between them: each slice at K F_i / F(t), F(t) = S e^(mu t).
        for (double lam : {0.25, 0.5, 0.9}) {
            const Time t = m.t1() + lam * (m.t2() - m.t1());
            const double F = m.fwd(t);
            const double w1 = s1.totalVariance(K * m.p1.forward / F);
            const double w2 = s2.totalVariance(K * m.p2.forward / F);
            const double expected = w1 + (w2 - w1) * lam;
            EXPECT_NEAR(fm->blackVariance(t, K, true), expected, 1e-13) << K << " " << t;
            // The fixed-strike reading is the same with no strike shift.
            const double f1 = s1.totalVariance(K), f2 = s2.totalVariance(K);
            EXPECT_NEAR(fs->blackVariance(t, K, true), f1 + (f2 - f1) * lam, 1e-13);
        }
        // Along a forward line with the surface's drift k is fixed: the
        // variance is linear in t with slope delta / (T2 - T1).
        const double k = std::log(K / m.fwd(m.t1()));
        const Time ta = m.t1() + 0.2 * (m.t2() - m.t1()), tb = m.t1() + 0.7 * (m.t2() - m.t1());
        const double wa = fm->blackVariance(ta, m.fwd(ta) * std::exp(k), true);
        const double wb = fm->blackVariance(tb, m.fwd(tb) * std::exp(k), true);
        EXPECT_NEAR((wb - wa) / (tb - ta), m.delta / (m.t2() - m.t1()), 1e-10) << K;
    }
    // The two readings differ between the pillars (the forward moves by
    // ln(F2 / F1) = 0.1).
    const Time mid = 0.5 * (m.t1() + m.t2());
    EXPECT_GT(
        std::fabs(fm->blackVariance(mid, 80.0, true) - fs->blackVariance(mid, 80.0, true)), 1e-5);
    // Before the first slice and after the last: the slice at the shifted
    // strike scaled by t / T.
    const double F0 = m.fwd(0.2);
    EXPECT_NEAR(
        fm->blackVariance(0.2, 90.0, true),
        s1.totalVariance(90.0 * (m.p1.forward / F0)) * 0.2 / m.t1(), 1e-14);
    const double F3 = m.fwd(2.0);
    EXPECT_NEAR(
        fm->blackVariance(2.0, 90.0, true),
        s2.totalVariance(90.0 * (m.p2.forward / F3)) * 2.0 / m.t2(), 1e-12);
}

TEST(ForwardMoneynessSurface, CalendarOrderedAtFixedMoneynessGivesNoNegativeDupireNumerator) {
    DriftMarket m;
    const auto fs = m.surface(SviTimeReading::FixedStrike);
    const auto fm = m.surface(SviTimeReading::FixedForwardMoneyness);
    // The calibration market: spot 95, r = mu, q = 0 (the slices' forwards).
    const ScanResult oldOwn = dupireScan(fs, m.spot, m.mu, 0.0, m);
    const ScanResult newOwn = dupireScan(fm, m.spot, m.mu, 0.0, m);
    EXPECT_GT(oldOwn.refused, 0u) << "the fixed-strike reading is expected to refuse";
    EXPECT_EQ(newOwn.refused, 0u) << "of " << newOwn.points;
    // A note's own dividend curve: the process forward drifts 50 bp/yr
    // slower than the calibration forwards (the surface stays where it is).
    const ScanResult oldNote = dupireScan(fs, m.spot, m.mu, 0.005, m);
    const ScanResult newNote = dupireScan(fm, m.spot, m.mu, 0.005, m);
    EXPECT_GT(oldNote.refused, 0u);
    EXPECT_EQ(newNote.refused, 0u) << "of " << newNote.points;
}

TEST(ForwardMoneynessSurface, GridOnCurvesMatchesGridOnNodeForwards) {
    DriftMarket m;
    // A grid sampled from the two slices; the same rows read against node
    // forwards on the slices' forwards (and the spot) and against spot
    // D_q / D_r on flat curves with the same drift.
    std::vector<double> strikes;
    for (int j = 0; j <= 40; ++j)
        strikes.push_back(50.0 + 5.0 * j);
    const SviSmileSlice s1(m.p1), s2(m.p2);
    std::vector<std::vector<double>> rows(2, std::vector<double>(strikes.size()));
    for (std::size_t j = 0; j < strikes.size(); ++j) {
        rows[0][j] = std::sqrt(s1.totalVariance(strikes[j]) / m.t1());
        rows[1][j] = std::sqrt(s2.totalVariance(strikes[j]) / m.t2());
    }
    const auto node = makeCubicStrikeLinearTimeSurface(
        m.asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, m.dc, {m.d1, m.d2}, strikes, rows,
        std::make_shared<NodeSurfaceForward>(
            std::vector<double>{m.t1(), m.t2()}, std::vector<double>{m.p1.forward, m.p2.forward},
            m.spot));
    const auto curve = makeCubicStrikeLinearTimeSurface(
        m.asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, m.dc, {m.d1, m.d2}, strikes, rows,
        std::make_shared<CurveSurfaceForward>(m.spot, m.flat(0.0), m.flat(m.mu)));
    const auto fixedK = makeCubicStrikeLinearTimeSurface(
        m.asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, m.dc, {m.d1, m.d2}, strikes, rows);
    for (Real K : {70.0, 100.0, 140.0}) {
        EXPECT_EQ(node->blackVariance(m.t1(), K, true), fixedK->blackVariance(m.t1(), K, true));
        for (Time t : {0.1, 0.8, 1.2, 2.0})
            EXPECT_NEAR(node->blackVariance(t, K, true), curve->blackVariance(t, K, true), 1e-12)
                << K << " " << t;
    }
    // The grid of an ordered surface: no refusal either.
    EXPECT_EQ(dupireScan(node, m.spot, m.mu, 0.0, m).refused, 0u);
    EXPECT_EQ(dupireScan(node, m.spot, m.mu, 0.005, m).refused, 0u);
}

TEST(ForwardMoneynessSurface, NoteLocalVolFollowsTheProcessForwardBetweenExpiries) {
    // Slices ordered at fixed moneyness by a thin margin (1 % forward vol),
    // read by a process whose forward meets the slices' forwards at the
    // expiries but bends between them (a dividend curve linear in zero
    // rate with a node inside the interval): against the surface's own
    // ln-linear forward the Dupire numerator picks up the skew times the
    // drift difference and turns negative in the put wing; against the
    // process-shaped forward (the note's reading) it is the slices'
    // difference again, and the slices are read exactly at the expiries.
    DriftMarket m;
    m.delta = 1e-4 * (m.t2() - m.t1());
    m.p2 = m.p1;
    m.p2.forward = m.fwd(m.t2());
    m.p2.a += m.delta;
    const auto fm = m.surface(SviTimeReading::FixedForwardMoneyness);
    const Date mid = m.asOf + 365;
    auto zc = std::make_shared<QuantLib::ZeroCurve>(
        std::vector<Date>{m.asOf, m.d1, mid, m.d2}, std::vector<Real>{0.0, 0.0, 0.03, 0.0}, m.dc);
    zc->enableExtrapolation();
    const Handle<QuantLib::YieldTermStructure> q(zc), r = m.flat(m.mu);
    // The process forward meets the slices' forwards at the expiries.
    EXPECT_NEAR(m.spot * q->discount(m.t1()) / r->discount(m.t1()), m.p1.forward, 1e-9);
    EXPECT_NEAR(m.spot * q->discount(m.t2()) / r->discount(m.t2()), m.p2.forward, 1e-9);

    auto scan = [&](const std::shared_ptr<QuantLib::BlackVolTermStructure>& vol) {
        QuantLib::LocalVolSurface lv(
            Handle<QuantLib::BlackVolTermStructure>(vol), r, q,
            Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(m.spot)));
        unsigned refused = 0;
        for (int i = 51; i <= 149; ++i)
            for (int j = 0; j <= 60; ++j) {
                try {
                    lv.localVol(0.01 * i, 60.0 * std::pow(2.0, j / 60.0), true);
                } catch (const std::exception&) {
                    ++refused;
                }
            }
        return refused;
    };
    EXPECT_GT(scan(fm), 0u) << "the surface's own forward is expected to refuse here";
    const auto reading = localVolReadingSurface(fm, q, r);
    ASSERT_NE(reading.get(), fm.get());
    EXPECT_EQ(scan(reading), 0u);
    for (Real K : {70.0, 100.0, 130.0}) {
        EXPECT_EQ(reading->blackVariance(m.t1(), K, true), fm->blackVariance(m.t1(), K, true));
        EXPECT_EQ(reading->blackVariance(m.t2(), K, true), fm->blackVariance(m.t2(), K, true));
    }
    // The note's local vol (edge clamp, cache) reads the same way; a
    // fixed-strike surface is left as it is.
    mc::BasketAsset asset = mc::MakeBasketAsset(
        "X", Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(m.spot)), q, r,
        Handle<QuantLib::BlackVolTermStructure>(fm));
    mc::LocalVolCache cache;
    cache.attach(asset);
    for (int i = 51; i <= 149; ++i)
        for (Real K : {65.0, 80.0, 110.0})
            EXPECT_NO_THROW(asset.localVol->localVol(0.01 * i, K, true)) << i << " " << K;
    const auto fs = m.surface(SviTimeReading::FixedStrike);
    EXPECT_EQ(localVolReadingSurface(fs, q, r).get(), fs.get());
}

}} // namespace quantra::testing
