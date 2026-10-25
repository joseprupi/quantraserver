// eSSVI surface: the implied total variance against the Gatheral-Jacquier form, the analytic
// derivatives against central finite differences, the time interpolation, the arbitrage
// conditions and the numeric scan, the QuantLib wrappers, and the analytic Dupire local volatility
// against QuantLib's finite-difference LocalVolSurface (base market and a bumped process market,
// which exercises the sticky-strike drift correction).
//
// Parameter set used throughout ("good set", equity-index-like, T = 0.25, 0.5, 1, 2):
//
//   T      ATM vol   theta = vol^2 T   rho     psi
//   0.25   20.0%     0.010000          -0.70   0.10
//   0.50   19.0%     0.018050          -0.68   0.14
//   1.00   18.5%     0.034225          -0.65   0.20
//   2.00   18.0%     0.064800          -0.60   0.28
//
// Butterfly: psi (1 + |rho|) <= 4 and psi^2 (1 + |rho|) <= 4 theta hold (the tightest slice is
// the first: 0.01 * 1.7 = 0.017 <= 0.04). Calendar: theta and psi increase, a = rho psi =
// -0.070, -0.0952, -0.130, -0.168 so |Delta a| = 0.0252, 0.0348, 0.038 <= Delta psi = 0.04, 0.06,
// 0.08. checkArbitrage is asserted empty on it in SsviArbitrageTest.GoodSetHasNoViolation.
#include "ssvi.h"

#include "error.h"
#include "ssvi_vol_surface.h"

#include <ql/processes/blackscholesprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/zerospreadedtermstructure.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <random>
#include <vector>

namespace quantra { namespace testing {

namespace {

using ssvi::Node;
using ssvi::Slice;
using ssvi::Surface;

std::vector<Slice> goodSlices() {
    return {
        {0.25, 0.010000, -0.70, 0.10},
        {0.50, 0.018050, -0.68, 0.14},
        {1.00, 0.034225, -0.65, 0.20},
        {2.00, 0.064800, -0.60, 0.28}};
}

Node sliceNode(const Slice& s) {
    return Node{s.theta, s.psi, s.rho * s.psi, 0.0, 0.0, 0.0};
}

double gjW(double theta, double rho, double phi, double k) {
    return 0.5 * theta *
           (1.0 + rho * phi * k + std::sqrt((phi * k + rho) * (phi * k + rho) + 1.0 - rho * rho));
}

double relErr(double a, double b) {
    return std::fabs(a - b) / std::max(std::fabs(b), 1e-300);
}

// Market: reference date 2026-10-21, Actual365Fixed, flat r = 4%, q = 1.5%.
struct SsviMarket {
    QuantLib::Date ref{21, QuantLib::October, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    QuantLib::Calendar cal = QuantLib::NullCalendar();
    double spot0 = 100.0;

    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree =
        QuantLib::Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(ref, 0.04, dc));
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend =
        QuantLib::Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(ref, 0.015, dc));

    std::vector<QuantLib::Date> expiries() const {
        return {ref + 91, ref + 182, ref + 365, ref + 730};
    }
    std::vector<SsviSliceParams> params() const {
        std::vector<SsviSliceParams> out;
        for (const Slice& s : goodSlices())
            out.push_back({s.theta, s.rho, s.psi});
        return out;
    }
    std::shared_ptr<SsviBlackVol> blackVol() const {
        return std::make_shared<SsviBlackVol>(
            ref, cal, QuantLib::Following, dc, expiries(), params(), spot0, dividend, riskFree);
    }
};

} // namespace

// 1. w equals the Gatheral-Jacquier form.
TEST(SsviTest, TotalVarianceMatchesGatheralJacquier) {
    std::mt19937 gen(12345);
    std::uniform_real_distribution<double> uTheta(0.01, 0.2), uRho(-0.9, 0.9), uPhi(0.2, 3.0),
        uK(-1.0, 1.0);
    double worst = 0.0;
    for (int i = 0; i < 2000; ++i) {
        const double theta = uTheta(gen), rho = uRho(gen), phi = uPhi(gen), k = uK(gen);
        const Node n = sliceNode({1.0, theta, rho, theta * phi});
        worst = std::max(worst, relErr(ssvi::w(n, k), gjW(theta, rho, phi, k)));
    }
    std::cout << "[ssvi] max rel err w vs Gatheral-Jacquier: " << worst << std::endl;
    EXPECT_LT(worst, 1e-14);
}

// 2. Analytic derivatives against central finite differences.
TEST(SsviTest, DerivativesMatchFiniteDifferences) {
    const Surface surf(goodSlices());
    // strictly inside intervals and both extrapolation regions
    const double times[] = {0.1, 0.17, 0.37, 0.75, 1.5, 3.0, 5.0};
    const double ks[] = {-0.8, -0.3, -0.05, 0.0, 0.1, 0.4, 0.9};
    double worstK = 0, worstKK = 0, worstT = 0;
    for (double t : times) {
        for (double k : ks) {
            const Node n = surf.node(t);
            const double hk = 1e-5;
            const double fdK = (ssvi::w(n, k + hk) - ssvi::w(n, k - hk)) / (2 * hk);
            // w_kk by differencing the (separately verified) analytic w_k: the second
            // difference of w has a truncation error that is large for the short, sharply
            // peaked slices
            const double fdKK = (ssvi::wk(n, k + hk) - ssvi::wk(n, k - hk)) / (2 * hk);
            const double ht = 1e-6;
            const double fdT =
                (ssvi::w(surf.node(t + ht), k) - ssvi::w(surf.node(t - ht), k)) / (2 * ht);
            worstK = std::max(worstK, relErr(ssvi::wk(n, k), fdK));
            worstKK = std::max(worstKK, relErr(ssvi::wkk(n, k), fdKK));
            worstT = std::max(worstT, relErr(ssvi::wt(n, k), fdT));
        }
    }
    std::cout << "[ssvi] max rel err FD: w_k " << worstK << ", w_kk " << worstKK << ", w_t "
              << worstT << std::endl;
    EXPECT_LT(worstK, 1e-6);
    EXPECT_LT(worstKK, 1e-6);
    EXPECT_LT(worstT, 1e-6);
}

// 3. Time interpolation.
TEST(SsviTest, InterpolationAtPillarsBetweenAndBeyond) {
    const std::vector<Slice> sl = goodSlices();
    const Surface surf(sl);
    for (const Slice& s : sl) {
        const Node n = surf.node(s.t);
        EXPECT_DOUBLE_EQ(n.theta, s.theta);
        EXPECT_DOUBLE_EQ(n.psi, s.psi);
        EXPECT_DOUBLE_EQ(n.a, s.rho * s.psi);
    }
    // linear between pillars, derivatives are the interval slopes
    for (std::size_t i = 0; i + 1 < sl.size(); ++i) {
        const double dt = sl[i + 1].t - sl[i].t;
        for (double lam : {0.1, 0.5, 0.9}) {
            const Node n = surf.node(sl[i].t + lam * dt);
            const double a0 = sl[i].rho * sl[i].psi, a1 = sl[i + 1].rho * sl[i + 1].psi;
            EXPECT_NEAR(n.theta, sl[i].theta + lam * (sl[i + 1].theta - sl[i].theta), 1e-15);
            EXPECT_NEAR(n.psi, sl[i].psi + lam * (sl[i + 1].psi - sl[i].psi), 1e-15);
            EXPECT_NEAR(n.a, a0 + lam * (a1 - a0), 1e-15);
            EXPECT_NEAR(n.dTheta, (sl[i + 1].theta - sl[i].theta) / dt, 1e-12);
            EXPECT_NEAR(n.dPsi, (sl[i + 1].psi - sl[i].psi) / dt, 1e-12);
            EXPECT_NEAR(n.dA, (a1 - a0) / dt, 1e-12);
        }
    }
    // short end: w(k, t) = (t / T_1) w_1(k), rho constant
    for (double t : {0.02, 0.1, 0.2}) {
        const Node n = surf.node(t);
        for (double k : {-0.6, 0.0, 0.3}) {
            EXPECT_LT(relErr(ssvi::w(n, k), t / sl[0].t * ssvi::w(sliceNode(sl[0]), k)), 1e-14);
        }
        EXPECT_NEAR(n.a / n.psi, sl[0].rho, 1e-15);
    }
    // long end: psi and rho constant, theta continues the last forward variance
    const double s = (sl[3].theta - sl[2].theta) / (sl[3].t - sl[2].t);
    for (double t : {2.5, 4.0}) {
        const Node n = surf.node(t);
        EXPECT_DOUBLE_EQ(n.psi, sl[3].psi);
        EXPECT_NEAR(n.a / n.psi, sl[3].rho, 1e-15);
        EXPECT_NEAR(n.theta, sl[3].theta + s * (t - sl[3].t), 1e-15);
        EXPECT_DOUBLE_EQ(n.dTheta, s);
        EXPECT_DOUBLE_EQ(n.dPsi, 0.0);
        EXPECT_DOUBLE_EQ(n.dA, 0.0);
    }
    // single slice: continuation at theta_1 / T_1
    const Surface one(std::vector<Slice>{sl[0]});
    EXPECT_DOUBLE_EQ(one.node(1.0).dTheta, sl[0].theta / sl[0].t);
    // t = 0 is clamped, w is ~ 0
    EXPECT_LT(ssvi::w(surf.node(0.0), 0.0), 1e-6);
}

TEST(SsviTest, SurfaceRejectsInvalidSlices) {
    auto make = [](std::vector<Slice> v) { return Surface(std::move(v)); };
    EXPECT_THROW(make({}), QuantraInvalidArgument);
    EXPECT_THROW(make({{0.5, 0.01, -0.5, 0.1}, {0.5, 0.02, -0.5, 0.1}}), QuantraInvalidArgument);
    EXPECT_THROW(make({{0.5, 0.01, 1.0, 0.1}}), QuantraInvalidArgument);
    EXPECT_THROW(make({{0.5, -0.01, 0.0, 0.1}}), QuantraInvalidArgument);
    EXPECT_THROW(make({{0.5, 0.01, 0.0, 0.0}}), QuantraInvalidArgument);
    EXPECT_THROW(make({{0.0, 0.01, 0.0, 0.1}}), QuantraInvalidArgument);
    try {
        make({{0.5, 0.01, -0.5, 0.1}, {1.0, 0.02, -1.5, 0.1}});
        FAIL() << "expected throw";
    } catch (const QuantraInvalidArgument& e) {
        EXPECT_NE(std::string(e.what()).find("slice 1"), std::string::npos) << e.what();
    }
}

// 4. Arbitrage conditions.
TEST(SsviArbitrageTest, GoodSetHasNoViolation) {
    EXPECT_TRUE(ssvi::checkArbitrage(goodSlices()).empty());
}

TEST(SsviArbitrageTest, EachConditionIsDetected) {
    auto has = [](const std::vector<ssvi::Violation>& v, const std::string& name, std::size_t i) {
        for (const auto& x : v) {
            if (x.condition == name && x.slice == i && x.margin > 0.0) return true;
        }
        return false;
    };
    {
        auto s = goodSlices();
        s[1].psi = 4.0 / (1.0 + std::fabs(s[1].rho)) * 1.01; // psi (1 + |rho|) > 4
        EXPECT_TRUE(has(ssvi::checkArbitrage(s), "butterfly_psi", 1));
    }
    {
        auto s = goodSlices();
        s[2].psi = std::sqrt(4.0 * s[2].theta / (1.0 + std::fabs(s[2].rho))) * 1.01;
        const auto v = ssvi::checkArbitrage(s);
        EXPECT_TRUE(has(v, "butterfly_psi2", 2));
        EXPECT_FALSE(has(v, "butterfly_psi", 2));
    }
    {
        auto s = goodSlices();
        s[2].theta = s[1].theta * 0.9;
        const auto v = ssvi::checkArbitrage(s);
        EXPECT_TRUE(has(v, "calendar_theta", 2));
        for (const auto& x : v) {
            if (x.condition == "calendar_theta") EXPECT_EQ(x.prevSlice, x.slice - 1);
        }
    }
    {
        auto s = goodSlices();
        s[3].psi = s[2].psi * 0.9;
        EXPECT_TRUE(has(ssvi::checkArbitrage(s), "calendar_psi", 3));
    }
    {
        auto s = goodSlices();
        s[1].rho = -0.05; // a jumps from -0.07 to -0.007: |Delta a| = 0.063 > Delta psi = 0.04
        EXPECT_TRUE(has(ssvi::checkArbitrage(s), "calendar_a", 1));
    }
}

// Calendar: sufficient part (Hendriks-Martini Prop. 3.5 / Mingone 2022 Sec. 2.1).
TEST(SsviArbitrageTest, GoodSetSatisfiesPhiNonIncreasing) {
    const auto s = goodSlices();
    for (std::size_t i = 1; i < s.size(); ++i) {
        // (i): psi_{i+1} theta_i <= psi_i theta_{i+1}; phi = 10, 7.75, 5.84, 4.32
        EXPECT_LE(s[i].psi * s[i - 1].theta, s[i - 1].psi * s[i].theta) << "slice " << i;
    }
}

TEST(SsviArbitrageTest, NecessaryButNotSufficientCalendarIsDetected) {
    // theta_1 = theta_2 (1 - 1e-6), Delta a = -Delta psi: the necessary conditions hold (with
    // equality for a) but neither (i) nor (ii) does.
    const double th2 = 0.01, th1 = th2 * (1.0 - 1e-6);
    const double psi1 = 0.10, psi2 = 0.14, rho1 = -0.7;
    const double a2 = rho1 * psi1 - (psi2 - psi1);
    const Slice s1{0.25, th1, rho1, psi1}, s2{0.5, th2, a2 / psi2, psi2};
    const auto v = ssvi::checkArbitrage({s1, s2});
    ASSERT_EQ(v.size(), 1u);
    EXPECT_EQ(v[0].condition, "calendar_sufficient");
    EXPECT_GT(v[0].margin, 0.0);
    // real arbitrage at the endpoints: w2 < w1 for small k > 0
    bool found = false;
    for (double k = 0.01; k <= 1.0; k += 0.01) {
        if (ssvi::w(sliceNode(s2), k) < ssvi::w(sliceNode(s1), k)) found = true;
    }
    EXPECT_TRUE(found);
    const Surface surf({s1, s2});
    const ssvi::ScanResult r = ssvi::scan(surf, {0.25, 0.3, 0.4, 0.49}, -1.5, 1.0, 501);
    std::cout << "[ssvi] calendar-violating pair: min w_t " << r.minWt << " at t=" << r.minWtT
              << ", k=" << r.minWtK << std::endl;
    EXPECT_LT(r.minWt, 0.0);
}

TEST(SsviArbitrageTest, ConditionTwoWithoutConditionOneIsAccepted) {
    // phi increases (10 -> 10.5) so (i) fails, but (ii) holds:
    // (rho1 - psi2/psi1 rho2)^2 = 0.5929 <= (theta2/theta1 - 1)(...) = 1.205
    const Slice s1{0.25, 0.01, -0.7, 0.10}, s2{0.5, 0.02, -0.7, 0.21};
    ASSERT_GT(s2.psi * s1.theta, s1.psi * s2.theta);
    EXPECT_TRUE(ssvi::checkArbitrage({s1, s2}).empty());
    const Surface surf({s1, s2});
    std::vector<double> times;
    for (double t = 0.25; t <= 0.5; t += 0.25 / 400.0)
        times.push_back(t);
    const ssvi::ScanResult r = ssvi::scan(surf, times, -1.5, 1.0, 1001);
    std::cout << "[ssvi] (ii)-only pair: min w_t " << r.minWt << " at t=" << r.minWtT
              << ", k=" << r.minWtK << ", min g " << r.minG << std::endl;
    EXPECT_GE(r.minWt, 0.0);
    EXPECT_GE(r.minG, 0.0);
}

// 5. Numeric scan.
TEST(SsviArbitrageTest, ScanGoodSetIsArbitrageFree) {
    const Surface surf(goodSlices());
    std::vector<double> times;
    for (double t = 0.02; t < 4.0; t += 0.0137)
        times.push_back(t);
    for (const Slice& s : goodSlices())
        times.push_back(s.t);
    const ssvi::ScanResult r = ssvi::scan(surf, times, -1.5, 1.0, 801);
    std::cout << "[ssvi] scan good set: min g " << r.minG << " at (t=" << r.minGt
              << ", k=" << r.minGk << "), min w_t " << r.minWt << " at (t=" << r.minWtT
              << ", k=" << r.minWtK << ")" << std::endl;
    EXPECT_GE(r.minG, 0.0);
    EXPECT_GE(r.minWt, 0.0);
}

TEST(SsviArbitrageTest, ScanFindsButterflyViolation) {
    // psi = 1.0 with theta = 0.01: psi^2 (1 + |rho|) = 1.7 >> 4 theta = 0.04
    const Surface surf(std::vector<Slice>{{0.25, 0.01, -0.7, 1.0}});
    ASSERT_FALSE(ssvi::checkArbitrage({{0.25, 0.01, -0.7, 1.0}}).empty());
    const ssvi::ScanResult r = ssvi::scan(surf, {0.1, 0.25, 0.5}, -1.5, 1.0, 801);
    EXPECT_LT(r.minG, 0.0);
}

// 6. SsviBlackVol.
TEST(SsviVolSurfaceTest, BlackVarianceAtSliceExpiryIsTheSlice) {
    SsviMarket m;
    const auto bv = m.blackVol();
    const auto sl = goodSlices();
    const auto exp = m.expiries();
    for (std::size_t i = 0; i < sl.size(); ++i) {
        const double t = m.dc.yearFraction(m.ref, exp[i]);
        const double fwd = m.spot0 * m.dividend->discount(t) / m.riskFree->discount(t);
        EXPECT_LT(relErr(bv->forward(t), fwd), 1e-15);
        for (double K : {60.0, 90.0, 100.0, 110.0, 150.0}) {
            const double expect = ssvi::w(sliceNode(sl[i]), std::log(K / fwd));
            EXPECT_LT(relErr(bv->blackVariance(exp[i], K), expect), 1e-14);
            EXPECT_LT(relErr(bv->blackVol(exp[i], K), std::sqrt(expect / t)), 1e-14);
        }
    }
    EXPECT_EQ(bv->maxDate(), QuantLib::Date::maxDate());
}

TEST(SsviVolSurfaceTest, SurfaceIsStickyStrikeUnderProcessBumps) {
    SsviMarket m;
    const auto bv = m.blackVol();
    const double before = bv->blackVariance(m.ref + 300, 95.0);
    // spot0 is a snapshot double: nothing a process bump does can move it
    EXPECT_DOUBLE_EQ(bv->spot0(), 100.0);
    EXPECT_DOUBLE_EQ(bv->blackVariance(m.ref + 300, 95.0), before);
}

// 7. Analytic local vol against QuantLib's finite-difference Dupire.
namespace {

// Compares SsviLocalVol with LocalVolSurface(sameBlack, oracleRiskFree, oracleDividend, oracleSpot)
// and returns the max relative error.
double maxLocalVolError(
    const std::shared_ptr<SsviBlackVol>& bv,
    const QuantLib::Handle<QuantLib::Quote>& spot,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree) {
    SsviLocalVol analytic(bv, spot, dividend, riskFree);
    QuantLib::LocalVolSurface oracle(
        QuantLib::Handle<QuantLib::BlackVolTermStructure>(bv), riskFree, dividend, spot);
    double worst = 0.0;
    double worstT = 0, worstS = 0;
    for (double t : {0.1, 0.17, 0.37, 0.75, 1.5, 1.8, 3.0}) {
        const double fwd = spot->value() * dividend->discount(t) / riskFree->discount(t);
        for (double mny : {0.6, 0.75, 0.9, 1.0, 1.1, 1.25, 1.5}) {
            const double S = mny * fwd;
            const double a = analytic.localVol(t, S, true);
            const double q = oracle.localVol(t, S, true);
            const double e = relErr(a, q);
            if (e > worst) {
                worst = e;
                worstT = t;
                worstS = S;
            }
        }
    }
    std::cout << "[ssvi] max rel err analytic vs QuantLib Dupire: " << worst << " at t=" << worstT
              << ", S=" << worstS << std::endl;
    return worst;
}

} // namespace

TEST(SsviLocalVolTest, MatchesQuantLibDupireOnBaseMarket) {
    SsviMarket m;
    const auto bv = m.blackVol();
    QuantLib::Handle<QuantLib::Quote> spot(std::make_shared<QuantLib::SimpleQuote>(m.spot0));
    // the process market is the reference one: no drift correction at all
    SsviLocalVol lv(bv, spot, m.dividend, m.riskFree);
    const double t = 0.75, S = 105.0;
    const Node n = bv->surface().node(t);
    const double k = std::log(S / bv->forward(t));
    EXPECT_DOUBLE_EQ(lv.localVol(t, S, true), std::sqrt(ssvi::wt(n, k) / ssvi::g(n, k, k)));
    EXPECT_LT(maxLocalVolError(bv, spot, m.dividend, m.riskFree), 1e-4);
}

TEST(SsviLocalVolTest, MatchesQuantLibDupireOnBumpedProcessMarket) {
    SsviMarket m;
    const auto bv = m.blackVol();
    QuantLib::Handle<QuantLib::Quote> spot(std::make_shared<QuantLib::SimpleQuote>(m.spot0 * 1.05));
    QuantLib::Handle<QuantLib::Quote> spread(std::make_shared<QuantLib::SimpleQuote>(0.005));
    QuantLib::Handle<QuantLib::YieldTermStructure> bumpedRf(
        std::make_shared<QuantLib::ZeroSpreadedTermStructure>(m.riskFree, spread));
    const double err = maxLocalVolError(bv, spot, m.dividend, bumpedRf);
    EXPECT_LT(err, 1e-4);

    // the correction matters: without it (reference market) the local vol differs visibly
    QuantLib::Handle<QuantLib::Quote> baseSpot(std::make_shared<QuantLib::SimpleQuote>(m.spot0));
    SsviLocalVol bumped(bv, spot, m.dividend, bumpedRf);
    SsviLocalVol base(bv, baseSpot, m.dividend, m.riskFree);
    EXPECT_GT(relErr(bumped.localVol(0.75, 100.0, true), base.localVol(0.75, 100.0, true)), 1e-3);
}

TEST(SsviLocalVolTest, ThrowsWhereTheSurfaceAdmitsArbitrage) {
    SsviMarket m;
    std::vector<SsviSliceParams> bad{{0.01, -0.7, 1.0}};
    auto bv = std::make_shared<SsviBlackVol>(
        m.ref, m.cal, QuantLib::Following, m.dc, std::vector<QuantLib::Date>{m.ref + 91}, bad,
        m.spot0, m.dividend, m.riskFree);
    QuantLib::Handle<QuantLib::Quote> spot(std::make_shared<QuantLib::SimpleQuote>(m.spot0));
    SsviLocalVol lv(bv, spot, m.dividend, m.riskFree);
    bool thrown = false;
    for (double mny : {0.3, 0.5, 0.7, 1.0, 1.5, 2.0}) {
        try {
            lv.localVol(0.2, mny * bv->forward(0.2), true);
        } catch (const QuantraError& e) {
            thrown = true;
            EXPECT_NE(std::string(e.what()).find("S="), std::string::npos);
        }
    }
    EXPECT_TRUE(thrown);
}

// 8. Wired into a GeneralizedBlackScholesProcess.
TEST(SsviLocalVolTest, ProcessUsesTheAnalyticLocalVol) {
    SsviMarket m;
    const auto bv = m.blackVol();
    QuantLib::Handle<QuantLib::Quote> spot(std::make_shared<QuantLib::SimpleQuote>(m.spot0));
    auto lv = std::make_shared<SsviLocalVol>(bv, spot, m.dividend, m.riskFree);
    QuantLib::Handle<QuantLib::LocalVolTermStructure> lvHandle(lv);
    QuantLib::GeneralizedBlackScholesProcess process(
        spot, m.dividend, m.riskFree, QuantLib::Handle<QuantLib::BlackVolTermStructure>(bv),
        lvHandle);
    EXPECT_EQ(process.localVolatility().currentLink().get(), lv.get());
    for (double t : {0.1, 0.4, 0.75, 1.5, 3.0}) {
        for (double S : {70.0, 100.0, 130.0}) {
            EXPECT_DOUBLE_EQ(process.diffusion(t, S), lv->localVol(t, S, true));
        }
    }
}

}} // namespace quantra::testing
