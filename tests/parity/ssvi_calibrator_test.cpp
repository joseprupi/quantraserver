// eSSVI slice calibrator: recovery of a known surface from option prices (with a quote forward
// that differs from the curve's, so the put-call-parity forward and the implied dividend yield
// are exercised) and from implied vols, robustness to noise, calendar arbitrage in the data,
// too-steep wings, the weighting modes and the 400 paths.
//
// Truth: the "good set" of ssvi_test.cpp (T ~ 0.25, 0.5, 1, 2 from reference + 91, 182, 365, 730
// days; theta = 0.010000, 0.018050, 0.034225, 0.064800; rho = -0.70, -0.68, -0.65, -0.60; psi =
// 0.10, 0.14, 0.20, 0.28), quoted at 15 strikes from 50% to 150% of the generating forward plus
// one strike exactly at the forward (k = 0, the calibration anchor). Generating dividend yield
// 2.5% against 1.5% on the curve, risk-free 4%, spot 100.
#include "ssvi_calibrator.h"

#include "error.h"

#include <ql/pricingengines/blackformula.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <vector>

namespace quantra { namespace testing {

namespace {

using ssvi::CalibrationInput;
using ssvi::CalibrationResult;
using ssvi::OptionQuote;
using ssvi::QuoteWeighting;

struct CalibMarket {
    QuantLib::Date ref{21, QuantLib::October, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    double spot = 100.0;
    double qGen = 0.025;
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree =
        QuantLib::Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(ref, 0.04, dc));
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend =
        QuantLib::Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(ref, 0.015, dc));
    std::vector<int> days{91, 182, 365, 730};

    std::vector<SsviSliceParams> truth() const {
        return {
            {0.010000, -0.70, 0.10},
            {0.018050, -0.68, 0.14},
            {0.034225, -0.65, 0.20},
            {0.064800, -0.60, 0.28}};
    }
    // Non-empty: strikes are F exp(offsets[i]) (0.5 + j / 14) on slice i and there is no strike
    // at the forward, so the anchor has k* != 0.
    std::vector<double> offsets;
    double time(std::size_t i) const { return dc.yearFraction(ref, ref + days[i]); }
    double genForward(std::size_t i) const {
        return spot * std::exp(-qGen * time(i)) / riskFree->discount(time(i));
    }

    // noise: +- noise vol alternating with the strike index (vol quotes only)
    CalibrationInput input(
        const std::vector<SsviSliceParams>& p, bool prices, double noise = 0.0) const {
        CalibrationInput in;
        in.referenceDate = ref;
        in.dayCounter = dc;
        in.spot = spot;
        in.discount = riskFree;
        in.dividend = dividend;
        for (std::size_t i = 0; i < p.size(); ++i) {
            const double T = time(i), F = genForward(i), D = riskFree->discount(T);
            const ssvi::Node n{p[i].theta, p[i].psi, p[i].rho * p[i].psi, 0, 0, 0};
            std::vector<double> strikes;
            const double shift = std::exp(offsets.empty() ? 0.0 : offsets[i]);
            for (int j = 0; j < 15; ++j)
                strikes.push_back(shift * F * (0.5 + j / 14.0));
            if (offsets.empty()) strikes.push_back(F);
            for (std::size_t j = 0; j < strikes.size(); ++j) {
                const double K = strikes[j];
                const double sd = std::sqrt(ssvi::w(n, std::log(K / F)));
                for (auto type : {QuantLib::Option::Call, QuantLib::Option::Put}) {
                    OptionQuote q;
                    q.expiry = ref + days[i];
                    q.strike = K;
                    q.type = type;
                    if (prices) {
                        q.price = QuantLib::blackFormula(type, K, F, sd, D);
                    } else {
                        const double sign = (j % 2 == 0 && j + 1 < strikes.size()) ? 1.0 : -1.0;
                        q.impliedVol = sd / std::sqrt(T) + sign * noise;
                    }
                    in.quotes.push_back(q);
                }
            }
        }
        return in;
    }
};

void expectRecovered(const CalibMarket& m, const CalibrationResult& r, double tol) {
    const auto truth = m.truth();
    ASSERT_EQ(r.params.size(), truth.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < truth.size(); ++i) {
        worst = std::max(
            {worst, std::fabs(r.params[i].theta - truth[i].theta),
             std::fabs(r.params[i].rho - truth[i].rho), std::fabs(r.params[i].psi - truth[i].psi)});
    }
    std::cout << "[ssvi-calib] max abs parameter error: " << worst << ", rmse " << r.rmseVolPts
              << " vol pts" << std::endl;
    EXPECT_LE(worst, tol);
    EXPECT_TRUE(r.violations.empty());
    EXPECT_GE(r.scan.minG, 0.0);
    EXPECT_GE(r.scan.minWt, 0.0);
}

} // namespace

// 1. Recovery from prices, quote forward != curve forward.
TEST(SsviCalibTest, RecoversTruthFromPricesAndParityForward) {
    CalibMarket m;
    const CalibrationResult r = ssvi::calibrate(m.input(m.truth(), true));
    expectRecovered(m, r, 5e-11); // exact data: nothing rejected, recovery unchanged
    for (const auto& d : r.slices)
        EXPECT_EQ(d.parityPairsRejected, 0u);
    double worstF = 0.0, worstQ = 0.0;
    for (std::size_t i = 0; i < 4; ++i) {
        worstF = std::max(worstF, std::fabs(r.fitForwards[i] / m.genForward(i) - 1.0));
        worstQ = std::max(worstQ, std::fabs(r.impliedDividendYields[i] - m.qGen));
        EXPECT_FALSE(r.slices[i].anchorClamped);
        EXPECT_NEAR(r.slices[i].anchorK, 0.0, 1e-9);
        EXPECT_NE(r.fitForwards[i], r.curveForwards[i]);
        EXPECT_GE(r.slices[i].marginButterflyPsi, 0.0);
        EXPECT_GE(r.slices[i].marginButterflyPsi2, 0.0);
        EXPECT_GE(r.slices[i].marginCalendarTheta, 0.0);
        EXPECT_GE(r.slices[i].marginCalendarPsi, 0.0);
        EXPECT_GE(r.slices[i].marginCalendarA, 0.0);
    }
    std::cout << "[ssvi-calib] fit forward rel err " << worstF << ", implied q abs err " << worstQ
              << std::endl;
    EXPECT_LE(worstF, 1e-10);
    EXPECT_LE(worstQ, 1e-9);
    EXPECT_EQ(r.quotes.size(), 4u * 32u);
    EXPECT_EQ(r.excludedByMoneyness, 0u);
}

// 2. Recovery from implied vols (curve forward). The truth is quoted on the curve forward here.
TEST(SsviCalibTest, RecoversTruthFromImpliedVols) {
    CalibMarket m;
    m.qGen = 0.015; // generating forward = curve forward
    const CalibrationResult r = ssvi::calibrate(m.input(m.truth(), false));
    expectRecovered(m, r, 1e-6);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(r.fitForwards[i], r.curveForwards[i]);
    }
}

// 3. Deterministic noise.
TEST(SsviCalibTest, NoisyVolsFitWithinNoiseAndStayArbitrageFree) {
    CalibMarket m;
    m.qGen = 0.015;
    const CalibrationResult r = ssvi::calibrate(m.input(m.truth(), false, 0.003));
    std::cout << "[ssvi-calib] noisy: rmse " << r.rmseVolPts << ", max " << r.maxAbsVolPts
              << " vol pts, min g " << r.scan.minG << std::endl;
    EXPECT_LE(r.rmseVolPts, 0.35);
    EXPECT_TRUE(r.violations.empty());
    EXPECT_GE(r.scan.minG, 0.0);
    EXPECT_GE(r.scan.minWt, 0.0);
}

// 4. Calendar arbitrage at the money in the data.
TEST(SsviCalibTest, CalendarArbitrageInDataIsClamped) {
    CalibMarket m;
    m.qGen = 0.015;
    auto p = m.truth();
    p[1].theta = 0.8 * p[0].theta; // second expiry below the first at the money
    const CalibrationResult r = ssvi::calibrate(m.input(p, false));
    EXPECT_TRUE(r.violations.empty());
    EXPECT_TRUE(r.slices[1].anchorClamped);
    EXPECT_FALSE(r.slices[0].anchorClamped);
    EXPECT_GE(r.params[1].theta, r.params[0].theta);
    EXPECT_GE(r.scan.minG, 0.0);
    EXPECT_GE(r.scan.minWt, 0.0);
    for (std::size_t i = 0; i < 2; ++i) {
        std::cout << "[ssvi-calib] clamped slice " << i << ": theta " << r.params[i].theta
                  << ", rho " << r.params[i].rho << ", psi " << r.params[i].psi << std::endl;
    }
    std::cout << "[ssvi-calib] clamped slice 1: anchorTheta " << r.slices[1].anchorTheta
              << ", fit rmse " << r.slices[1].rmseVolPts << " vol pts" << std::endl;
}

// 5. Wings steeper than the butterfly bound.
TEST(SsviCalibTest, TooSteepWingsFitAtTheButterflyBound) {
    CalibMarket m;
    m.qGen = 0.015;
    auto p = m.truth();
    p[0].psi = 0.30; // bound sqrt(4 theta / (1 + |rho|)) = 0.153
    const CalibrationResult r = ssvi::calibrate(m.input(p, false));
    EXPECT_TRUE(r.violations.empty());
    EXPECT_GE(r.scan.minG, 0.0);
    EXPECT_GE(r.scan.minWt, 0.0);
    const auto& d = r.slices[0];
    std::cout << "[ssvi-calib] steep wings: psi " << r.params[0].psi << ", rho " << r.params[0].rho
              << ", margins " << d.marginButterflyPsi << " / " << d.marginButterflyPsi2 << ", rmse "
              << d.rmseVolPts << " vol pts" << std::endl;
    EXPECT_GE(d.marginButterflyPsi, 0.0);
    EXPECT_GE(d.marginButterflyPsi2, 0.0);
    // at the bound: one of the two butterfly margins is ~0 relative to its scale
    EXPECT_LT(
        std::min(d.marginButterflyPsi / 4.0, d.marginButterflyPsi2 / (4.0 * r.params[0].theta)),
        1e-8);
    EXPECT_GT(d.rmseVolPts, 0.01); // cannot fit the steep data
}

// 6. Weighting modes and 400s.
TEST(SsviCalibTest, WeightingModesRun) {
    CalibMarket m;
    m.qGen = 0.015;
    for (auto mode : {QuoteWeighting::Uniform, QuoteWeighting::Vega, QuoteWeighting::BidAsk}) {
        CalibrationInput in = m.input(m.truth(), false, 0.002);
        in.weighting = mode;
        if (mode == QuoteWeighting::BidAsk) {
            // spreads widen away from the money; every third quote has none
            for (std::size_t i = 0; i < in.quotes.size(); ++i) {
                if (i % 3 == 0) continue;
                const double half = 0.002 + 0.01 * std::fabs(std::log(in.quotes[i].strike / 100.0));
                in.quotes[i].bid = *in.quotes[i].impliedVol - half;
                in.quotes[i].ask = *in.quotes[i].impliedVol + half;
            }
        }
        const CalibrationResult r = ssvi::calibrate(in);
        std::cout << "[ssvi-calib] weighting " << static_cast<int>(mode) << ": rmse "
                  << r.rmseVolPts << " vol pts" << std::endl;
        EXPECT_TRUE(r.violations.empty());
        EXPECT_GE(r.scan.minWt, 0.0);
        EXPECT_LT(r.rmseVolPts, 0.5);
    }
    // BidAsk on price quotes, spread in price units
    CalibrationInput pin = m.input(m.truth(), true);
    pin.weighting = QuoteWeighting::BidAsk;
    for (auto& q : pin.quotes) {
        q.bid = *q.price * 0.99;
        q.ask = *q.price * 1.01;
    }
    const CalibrationResult pr = ssvi::calibrate(pin);
    EXPECT_TRUE(pr.violations.empty());
    EXPECT_GE(pr.scan.minWt, 0.0);
}

TEST(SsviCalibTest, InvalidInputsAreClientErrors) {
    CalibMarket m;
    m.qGen = 0.015;
    {
        CalibrationInput in = m.input(m.truth(), false);
        in.weighting = QuoteWeighting::BidAsk;
        EXPECT_THROW(ssvi::calibrate(in), QuantraInvalidArgument);
    }
    {
        CalibrationInput in = m.input(m.truth(), false);
        in.quotes[5].price = 1.0; // both price and impliedVol
        try {
            ssvi::calibrate(in);
            FAIL() << "expected throw";
        } catch (const QuantraInvalidArgument& e) {
            EXPECT_NE(std::string(e.what()).find("quote 5"), std::string::npos) << e.what();
        }
    }
    {
        CalibrationInput in = m.input(m.truth(), true);
        // in-the-money call (K = 0.71 F) far below intrinsic: excluded, not a 400
        in.quotes[6].price = 1.0;
        const CalibrationResult r = ssvi::calibrate(in);
        EXPECT_FALSE(r.quotes[6].used);
        EXPECT_EQ(r.quotes[6].exclusionReason, "price_outside_bounds");
        EXPECT_EQ(r.quotes[7].exclusionReason, "parity_outlier");
        EXPECT_EQ(r.slices[0].parityPairsRejected, 1u);
        expectRecovered(m, r, 1e-6);
    }
    {
        CalibrationInput in = m.input(m.truth(), false);
        in.quotes[0].expiry = m.ref; // T = 0
        EXPECT_THROW(ssvi::calibrate(in), QuantraInvalidArgument);
    }
}

}} // namespace quantra::testing

namespace quantra { namespace testing {
namespace {

CalibMarket offForwardMarket() {
    CalibMarket m;
    m.offsets = {0.003, 0.005, 0.007, -0.004};
    return m;
}

void expectArbitrageFree(const CalibrationResult& r) {
    EXPECT_TRUE(r.violations.empty());
    EXPECT_GE(r.scan.minG, 0.0);
    EXPECT_GE(r.scan.minWt, 0.0);
}

} // namespace

// The exact anchor inversion: w(k; anchorTheta(W, rho, k, psi), rho, psi) = W.
TEST(SsviCalibTest, AnchorThetaInvertsTheSliceVariance) {
    unsigned s = 12345u;
    auto uni = [&](double a, double b) {
        s = s * 1664525u + 1013904223u;
        return a + (b - a) * (s / 4294967296.0);
    };
    for (int i = 0; i < 2000; ++i) {
        const double theta = uni(0.001, 0.5), rho = uni(-0.99, 0.99), psi = uni(0.01, 2.0);
        const double k = uni(-1.0, 1.0);
        const ssvi::Node n{theta, psi, rho * psi, 0, 0, 0};
        const double W = ssvi::w(n, k);
        const double back = ssvi::anchorTheta(W, rho, k, psi);
        // Backward error: the slice rebuilt from the inverted theta has variance W to 1e-13 (|rho| up to 0.99).
        // The relative error of theta itself is conditioned by psi k / theta (up to ~1e3 here) and
        // stays below 1e-12.
        const ssvi::Node nb{back, psi, rho * psi, 0, 0, 0};
        EXPECT_NEAR(ssvi::w(nb, k) / W, 1.0, 1e-13)
            << "theta " << theta << " rho " << rho << " psi " << psi << " k " << k;
        EXPECT_NEAR(back / theta, 1.0, 1e-12)
            << "theta " << theta << " rho " << rho << " psi " << psi << " k " << k;
    }
}

// No strike at the forward, 0.3%-0.7% log-moneyness offsets on every slice.
TEST(SsviCalibTest, RecoversTruthWithNoStrikeAtTheForward) {
    for (bool prices : {true, false}) {
        CalibMarket m = offForwardMarket();
        if (!prices) m.qGen = 0.015;
        const CalibrationResult r = ssvi::calibrate(m.input(m.truth(), prices));
        std::cout << "[ssvi-calib] off-forward " << (prices ? "prices" : "vols") << ":\n";
        expectRecovered(m, r, 1e-9);
        for (std::size_t i = 0; i < 4; ++i) {
            EXPECT_GT(std::fabs(r.slices[i].anchorK), 1e-3);
            if (prices) {
                EXPECT_LE(std::fabs(r.fitForwards[i] / m.genForward(i) - 1.0), 1e-10);
            }
        }
    }
}

// The model reproduces the anchor quote exactly, noise or not.
TEST(SsviCalibTest, ModelVolEqualsMarketVolAtTheAnchor) {
    CalibMarket m = offForwardMarket();
    m.qGen = 0.015;
    const CalibrationResult r = ssvi::calibrate(m.input(m.truth(), false, 0.003));
    for (std::size_t s = 0; s < r.slices.size(); ++s) {
        if (r.slices[s].anchorClamped) continue;
        const ssvi::QuoteRow* anchor = nullptr;
        for (const ssvi::QuoteRow& row : r.quotes) {
            if (row.used && row.expiry == r.expiries[s] &&
                (!anchor || std::fabs(row.k) < std::fabs(anchor->k))) {
                anchor = &row;
            }
        }
        ASSERT_NE(anchor, nullptr);
        EXPECT_NEAR(anchor->modelVol, anchor->marketVol, 1e-12) << "slice " << s;
    }
    expectArbitrageFree(r);
}

// The binding constraint is, in turn, butterfly psi^2 (1 + |rho|) <= 4 theta (3), calendar
// theta > theta_prev (4) and calendar sufficient condition (i) (6).
TEST(SsviCalibTest, QuadraticConstraintsBindAndStayArbitrageFree) {
    { // 3: wings steeper than the butterfly bound, anchor off the forward
        CalibMarket m = offForwardMarket();
        m.qGen = 0.015;
        auto p = m.truth();
        p[0].psi = 0.30;
        const CalibrationResult r = ssvi::calibrate(m.input(p, false));
        const auto& d = r.slices[0];
        std::cout << "[ssvi-calib] bind 3: margins " << d.marginButterflyPsi << " / "
                  << d.marginButterflyPsi2 << " theta " << r.params[0].theta << std::endl;
        expectArbitrageFree(r);
        EXPECT_LT(d.marginButterflyPsi2 / (4.0 * r.params[0].theta), 1e-8);
        EXPECT_GT(d.marginButterflyPsi / 4.0, 1e-3);
    }
    { // 4: second expiry's theta below the first, but its anchor variance W above it (rho k* >
        // 0 lifts W by about 4%): theta(psi) < theta_prev at the true psi, so the calendar bound
        // theta(psi) > theta_prev binds without any clamping
        CalibMarket m = offForwardMarket();
        m.offsets[1] = -0.004;
        m.qGen = 0.015;
        auto p = m.truth();
        p[1].theta = 0.99 * p[0].theta;
        const CalibrationResult r = ssvi::calibrate(m.input(p, false));
        std::cout << "[ssvi-calib] bind 4: theta margin " << r.slices[1].marginCalendarTheta
                  << " theta " << r.params[1].theta << " clamped " << r.slices[1].anchorClamped
                  << std::endl;
        expectArbitrageFree(r);
        EXPECT_FALSE(r.slices[1].anchorClamped);
        // rho is free too, so the optimum sits near, not exactly on, the bound; the data's own
        // theta is 1% below theta_prev.
        EXPECT_LT(r.slices[1].marginCalendarTheta / r.params[1].theta, 1e-3);
    }
    { // 6: psi / theta increasing in the data, calendar (i) must hold in the fit
        CalibMarket m = offForwardMarket();
        m.qGen = 0.015;
        auto p = m.truth();
        p[0] = {0.0305, -0.60, 0.0915};
        p[1] = {0.0449, -0.60, 0.0915 * (0.0449 / 0.0305) * 1.15};
        p[2] = {0.0676, -0.60, p[1].psi * (0.0676 / 0.0449) * 1.15};
        p[3] = {0.1152, -0.60, p[2].psi * (0.1152 / 0.0676) * 1.15};
        const CalibrationResult r = ssvi::calibrate(m.input(p, false));
        expectArbitrageFree(r);
        double tightest = 1.0;
        for (std::size_t i = 1; i < r.params.size(); ++i) {
            const double slack = (r.params[i - 1].psi * r.params[i].theta -
                                  r.params[i].psi * r.params[i - 1].theta) /
                                 (r.params[i - 1].psi * r.params[i].theta);
            std::cout << "[ssvi-calib] bind 6: slice " << i << " (i)-slack " << slack << std::endl;
            EXPECT_GE(slack, 0.0);
            tightest = std::min(tightest, slack);
        }
        EXPECT_LT(tightest, 1e-6);
    }
}

}} // namespace quantra::testing
