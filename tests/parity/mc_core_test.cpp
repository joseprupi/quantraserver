// Shared Monte Carlo core (src/mc: basket_model, time_grid, path_engine):
// collapse tests against closed forms, with tolerances stated as multiples
// of the standard error the engine reports (a test fails when that standard
// error is zero or not finite), plus determinism, variance reduction, the
// convergence diagnostic, sample paths, the time grid and the 400 contract.
//
//  a. one asset, constant vol, ONE step to T = 1: European call and
//     cash-or-nothing digital vs AnalyticEuropeanEngine within 3 SE (the
//     step is exact for a strike-independent vol);
//  b. term vol (BlackVarianceCurve), one step to a T between the curve's
//     nodes: call vs Black at the integrated (interpolated-variance) vol;
//  c. two assets, rho in {-0.5, 0, 0.7}: P(S1 > K1 and S2 > K2) vs the
//     bivariate normal (BivariateCumulativeNormalDistributionWe04DP);
//  d. strike x expiry surface with a skew: 252 steps/year (Dupire local vol,
//     log-Euler) reproduces Black at the surface's own implied vol within
//     3 SE + a stated discretisation allowance, and ONE step is visibly
//     wrong (> 3 SE): a surface needs steps_per_year. exactStepCapable is
//     false for the surface, true for constant/term vol;
//  e. quanto: a foreign asset through buildQuantoDividendCurve has
//     E[S_T] = S exp((r_local - q - rho sigma_S sigma_FX) T) within 3 SE;
//  f. same seed -> identical means, different Mersenne-Twister seed ->
//     different means (Sobol: same stream for every seed, by design);
//     antithetic SE <= plain SE; Sobol error vs analytic < pseudo-random
//     error; running means at 1/4, 1/2, 3/4 present and within 4 SE of the
//     final mean (scaled for the sample count);
//  g. samplePaths = 3 returns three [asset][grid point] paths;
//  h. time grid: mandatory-only, refined (spacing <= 1 / stepsPerYear even
//     where QuantLib's rounding would not), indexOf, and the 400 rows;
//  i. exact-step table vs QuantLib's MultiPathGenerator on the market of the
//     four-index quanto autocall example (SOFR OIS discount curve, quanto
//     dividend curves, constant vols, 10 observation dates): every level of
//     every sample path and every accumulator mean are bit-identical, for
//     Sobol + bridge, Sobol and Mersenne Twister, with and without antithetic.
#include "basket_model.h"
#include "error.h"
#include "path_engine.h"
#include "quanto_builder.h"
#include "time_grid.h"

#include <ql/exercise.hpp>
#include <ql/indexes/ibor/sofr.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/distributions/bivariatenormaldistribution.hpp>
#include <ql/math/interpolations/bilinearinterpolation.hpp>
#include <ql/math/matrix.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancecurve.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancesurface.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/zerocurve.hpp>
#include <ql/time/calendars/japan.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/calendars/switzerland.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;
using QuantLib::Real;
using QuantLib::Size;

struct McMarket {
    Date valuation{15, QuantLib::October, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    QuantLib::Calendar cal = QuantLib::NullCalendar();

    Handle<QuantLib::Quote> spot(Real s) const {
        return Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(s));
    }
    Handle<QuantLib::YieldTermStructure> flat(QuantLib::Rate r) const {
        return Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(valuation, r, dc));
    }
    Handle<QuantLib::BlackVolTermStructure> constVol(QuantLib::Volatility v) const {
        return Handle<QuantLib::BlackVolTermStructure>(
            std::make_shared<QuantLib::BlackConstantVol>(valuation, cal, v, dc));
    }
    Date plusDays(int days) const { return valuation + days; }
};

mc::PathEngineConfig config(
    Size paths,
    mc::PathEngineConfig::Sequence sequence,
    bool antithetic,
    bool brownianBridge,
    unsigned long seed = 42) {
    mc::PathEngineConfig cfg;
    cfg.paths = paths;
    cfg.seed = seed;
    cfg.sequence = sequence;
    cfg.antithetic = antithetic;
    cfg.brownianBridge = brownianBridge;
    return cfg;
}

const Size kPaths = 1 << 14;
const auto kSobol = mc::PathEngineConfig::Sequence::Sobol;
const auto kPseudo = mc::PathEngineConfig::Sequence::PseudoRandom;

QuantLib::Matrix identity(Size n) {
    QuantLib::Matrix m(n, n, 0.0);
    for (Size i = 0; i < n; ++i) {
        m[i][i] = 1.0;
    }
    return m;
}

QuantLib::Matrix correlation2(Real rho) {
    QuantLib::Matrix m = identity(2);
    m[0][1] = m[1][0] = rho;
    return m;
}

// The standard error must be a usable number; the gap is then measured in
// multiples of it (plus an explicit absolute allowance where stated).
void expectWithinSE(
    const std::string& what,
    Real mean,
    Real se,
    Real reference,
    Real multiple,
    Real allowance = 0.0) {
    ASSERT_TRUE(std::isfinite(se)) << what << ": standard error is not finite";
    ASSERT_GT(se, 0.0) << what << ": standard error is zero";
    const Real gap = std::fabs(mean - reference);
    std::cout << "[ mc ] " << what << ": mean=" << mean << " reference=" << reference
              << " se=" << se << " gap/se=" << gap / se << std::endl;
    EXPECT_LE(gap, multiple * se + allowance)
        << what << ": mean " << mean << " vs reference " << reference << " gap " << gap << " = "
        << gap / se << " SE (limit " << multiple << " SE + " << allowance << ")";
}

Real analyticEuropean(
    const McMarket& m,
    const std::shared_ptr<QuantLib::StrikedTypePayoff>& payoff,
    const Date& expiry,
    const Handle<QuantLib::Quote>& spot,
    const Handle<QuantLib::YieldTermStructure>& dividend,
    const Handle<QuantLib::YieldTermStructure>& riskFree,
    const Handle<QuantLib::BlackVolTermStructure>& vol) {
    QuantLib::Settings::instance().evaluationDate() = m.valuation;
    auto process =
        std::make_shared<QuantLib::BlackScholesMertonProcess>(spot, dividend, riskFree, vol);
    QuantLib::VanillaOption option(payoff, std::make_shared<QuantLib::EuropeanExercise>(expiry));
    option.setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
    return option.NPV();
}

template <class Fn> void expectInvalidArgument(Fn&& fn, const std::vector<std::string>& needles) {
    try {
        fn();
        FAIL() << "expected QuantraInvalidArgument";
    } catch (const QuantraInvalidArgument& e) {
        const std::string msg = e.what();
        for (const auto& n : needles) {
            EXPECT_NE(msg.find(n), std::string::npos)
                << "message '" << msg << "' lacks '" << n << "'";
        }
    }
}

} // namespace

// a. One asset, constant vol, one step: the step is exact.
TEST(McCoreTest, OneAssetConstantVolOneStepMatchesAnalytic) {
    QuantLib::SavedSettings backup;
    McMarket m;
    const Real S = 100.0, K = 100.0, r = 0.05, q = 0.02, sigma = 0.20;
    const Date expiry = m.plusDays(365); // T = 1 on Actual/365 (Fixed)
    const auto spot = m.spot(S);
    const auto dividend = m.flat(q);
    const auto riskFree = m.flat(r);
    const auto vol = m.constVol(sigma);

    const auto asset = mc::MakeBasketAsset("SPX", spot, dividend, riskFree, vol);
    EXPECT_TRUE(asset.exactStepCapable);
    const auto processes = mc::BasketModel::build({asset}, identity(1));
    const auto grid = mc::BuildSimulationGrid(m.valuation, {expiry}, std::nullopt);
    ASSERT_EQ(grid.size(), 2u);
    const Size idx = grid.indexOf(expiry);
    const Real df = riskFree->discount(grid.grid[idx]);

    const Real callRef = analyticEuropean(
        m, std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, K), expiry, spot,
        dividend, riskFree, vol);
    const Real digitalRef = analyticEuropean(
        m, std::make_shared<QuantLib::CashOrNothingPayoff>(QuantLib::Option::Call, K, 1.0), expiry,
        spot, dividend, riskFree, vol);

    const mc::PathFunctor payoff = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
        const Real sT = path[0][idx];
        acc[0] = df * std::max(sT - K, 0.0);
        acc[1] = df * (sT > K ? 1.0 : 0.0);
    };

    for (const auto& [label, cfg] :
         {std::pair<std::string, mc::PathEngineConfig>{
              "pseudo-random", config(kPaths, kPseudo, false, false)},
          std::pair<std::string, mc::PathEngineConfig>{
              "sobol+bridge", config(kPaths, kSobol, false, true)}}) {
        const auto res = mc::RunPaths(processes, grid.grid, cfg, 2, payoff);
        EXPECT_EQ(res.paths, kPaths);
        EXPECT_EQ(res.samples, kPaths);
        expectWithinSE(
            "a. call constant vol one step (" + label + ")", res.mean[0], res.standardError[0],
            callRef, 3.0);
        expectWithinSE(
            "a. digital constant vol one step (" + label + ")", res.mean[1], res.standardError[1],
            digitalRef, 3.0);
    }
}

// b. Term vol: one step to a date between the curve nodes uses the
// interpolated (integrated) variance.
TEST(McCoreTest, TermVolOneStepUsesIntegratedVariance) {
    QuantLib::SavedSettings backup;
    McMarket m;
    const Real S = 100.0, K = 105.0, r = 0.03, q = 0.01;
    const std::vector<Date> dates = {m.plusDays(182), m.plusDays(365), m.plusDays(730)};
    const std::vector<QuantLib::Volatility> vols = {0.15, 0.25, 0.22};
    const Handle<QuantLib::BlackVolTermStructure> termVol(
        std::make_shared<QuantLib::BlackVarianceCurve>(m.valuation, dates, vols, m.dc, true));
    const Date expiry = m.plusDays(274);
    const Real T = 274.0 / 365.0;
    // Variance is linear in time between the nodes.
    const Real t1 = 182.0 / 365.0, t2 = 1.0;
    const Real v1 = 0.15 * 0.15 * t1, v2 = 0.25 * 0.25 * t2;
    const Real variance = v1 + (v2 - v1) * (T - t1) / (t2 - t1);
    EXPECT_NEAR(termVol->blackVariance(T, K), variance, 1e-12);

    const auto spot = m.spot(S);
    const auto dividend = m.flat(q);
    const auto riskFree = m.flat(r);
    const auto asset = mc::MakeBasketAsset("SX5E", spot, dividend, riskFree, termVol);
    EXPECT_TRUE(asset.exactStepCapable);
    const auto processes = mc::BasketModel::build({asset}, identity(1));
    const auto grid = mc::BuildSimulationGrid(m.valuation, {expiry}, std::nullopt);
    ASSERT_EQ(grid.size(), 2u);
    const Real df = riskFree->discount(T);
    const Real forward = S * dividend->discount(T) / df;
    const Real callRef =
        QuantLib::blackFormula(QuantLib::Option::Call, K, forward, std::sqrt(variance), df);

    const mc::PathFunctor payoff = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
        acc[0] = df * std::max(path[0].back() - K, 0.0);
    };
    const auto res =
        mc::RunPaths(processes, grid.grid, config(kPaths, kPseudo, false, false), 1, payoff);
    expectWithinSE("b. call term vol one step", res.mean[0], res.standardError[0], callRef, 3.0);
}

// c. Two correlated assets: joint exceedance probability vs the bivariate
// normal distribution.
TEST(McCoreTest, TwoAssetsJointExceedanceMatchesBivariateNormal) {
    McMarket m;
    const Real S1 = 100.0, S2 = 120.0, K1 = 95.0, K2 = 130.0;
    const Real r = 0.04, q1 = 0.02, q2 = 0.0, sigma1 = 0.20, sigma2 = 0.30, T = 1.0;
    const Date expiry = m.plusDays(365);
    const auto riskFree = m.flat(r);
    const auto a1 = mc::MakeBasketAsset("A1", m.spot(S1), m.flat(q1), riskFree, m.constVol(sigma1));
    const auto a2 = mc::MakeBasketAsset("A2", m.spot(S2), m.flat(q2), riskFree, m.constVol(sigma2));
    const auto grid = mc::BuildSimulationGrid(m.valuation, {expiry}, std::nullopt);
    const Size idx = grid.indexOf(expiry);

    const Real d1 =
        (std::log(S1 / K1) + (r - q1 - 0.5 * sigma1 * sigma1) * T) / (sigma1 * std::sqrt(T));
    const Real d2 =
        (std::log(S2 / K2) + (r - q2 - 0.5 * sigma2 * sigma2) * T) / (sigma2 * std::sqrt(T));

    const mc::PathFunctor indicator = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
        acc[0] = (path[0][idx] > K1 && path[1][idx] > K2) ? 1.0 : 0.0;
    };

    for (const Real rho : {-0.5, 0.0, 0.7}) {
        const QuantLib::BivariateCumulativeNormalDistributionWe04DP bivariate(rho);
        const Real probability = bivariate(d1, d2);
        const auto processes = mc::BasketModel::build({a1, a2}, correlation2(rho));
        for (const auto& [label, cfg] :
             {std::pair<std::string, mc::PathEngineConfig>{
                  "pseudo-random", config(kPaths, kPseudo, false, false)},
              std::pair<std::string, mc::PathEngineConfig>{
                  "sobol+bridge", config(kPaths, kSobol, false, true)}}) {
            const auto res = mc::RunPaths(processes, grid.grid, cfg, 1, indicator);
            expectWithinSE(
                "c. P(S1>K1, S2>K2) rho=" + std::to_string(rho) + " (" + label + ")", res.mean[0],
                res.standardError[0], probability, 3.0);
        }
    }
}

// d. A strike x expiry surface needs the fine grid: 252 steps/year
// reproduces the surface's implied vol, one step does not.
TEST(McCoreTest, SurfaceVolNeedsFineGrid) {
    McMarket m;
    const Real S = 100.0, K = 130.0, r = 0.05, q = 0.02, T = 1.0;
    const Date expiry = m.plusDays(365);
    // Implied variance linear in strike (a skew): bilinear interpolation of
    // variance reproduces it exactly between the nodes, so the Dupire local
    // vol is well defined everywhere. Constant extrapolation beyond the
    // strike range.
    const std::vector<Date> dates = {m.plusDays(182), m.plusDays(365), m.plusDays(730)};
    const std::vector<Real> strikes = {20, 40, 60, 80, 100, 120, 140, 160, 180, 200};
    QuantLib::Matrix vols(strikes.size(), dates.size());
    for (Size i = 0; i < strikes.size(); ++i) {
        const Real variance = 0.0625 - 0.0005 * (strikes[i] - 100.0);
        for (Size j = 0; j < dates.size(); ++j) {
            vols[i][j] = std::sqrt(variance);
        }
    }
    auto surface = std::make_shared<QuantLib::BlackVarianceSurface>(
        m.valuation, m.cal, dates, strikes, vols, m.dc,
        QuantLib::BlackVarianceSurface::ConstantExtrapolation,
        QuantLib::BlackVarianceSurface::ConstantExtrapolation);
    surface->setInterpolation<QuantLib::Bilinear>();
    const Handle<QuantLib::BlackVolTermStructure> surfaceVol(surface);
    EXPECT_FALSE(mc::IsExactStepCapable(surfaceVol));
    EXPECT_TRUE(mc::IsExactStepCapable(m.constVol(0.2)));

    const auto spot = m.spot(S);
    const auto dividend = m.flat(q);
    const auto riskFree = m.flat(r);
    const auto asset = mc::MakeBasketAsset("NKY", spot, dividend, riskFree, surfaceVol);
    EXPECT_FALSE(asset.exactStepCapable);
    EXPECT_FALSE(mc::BasketModel::allExactStepCapable({asset}));
    const auto processes = mc::BasketModel::build({asset}, identity(1));

    const Real impliedVol = surfaceVol->blackVol(T, K);
    EXPECT_NEAR(impliedVol, std::sqrt(0.0625 - 0.0005 * (K - 100.0)), 1e-12);
    const Real df = riskFree->discount(T);
    const Real forward = S * dividend->discount(T) / df;
    const Real callRef =
        QuantLib::blackFormula(QuantLib::Option::Call, K, forward, impliedVol * std::sqrt(T), df);

    const auto fine = mc::BuildSimulationGrid(m.valuation, {expiry}, Size(252));
    ASSERT_EQ(fine.size(), 253u);
    const Size fineIdx = fine.indexOf(expiry);
    const mc::PathFunctor finePayoff = [&](const QuantLib::MultiPath& path,
                                           std::vector<Real>& acc) {
        acc[0] = df * std::max(path[0][fineIdx] - K, 0.0);
    };
    const auto fineRes =
        mc::RunPaths(processes, fine.grid, config(kPaths, kSobol, false, true), 1, finePayoff);
    // Discretisation allowance for the log-Euler local-vol step at 1/252
    // year: 0.5% of the reference price (achieved: 0.1%).
    expectWithinSE(
        "d. call surface vol 252 steps/year", fineRes.mean[0], fineRes.standardError[0], callRef,
        3.0, 0.005 * callRef);

    const auto coarse = mc::BuildSimulationGrid(m.valuation, {expiry}, std::nullopt);
    ASSERT_EQ(coarse.size(), 2u);
    const mc::PathFunctor coarsePayoff = [&](const QuantLib::MultiPath& path,
                                             std::vector<Real>& acc) {
        acc[0] = df * std::max(path[0].back() - K, 0.0);
    };
    const auto coarseRes =
        mc::RunPaths(processes, coarse.grid, config(kPaths, kSobol, false, true), 1, coarsePayoff);
    ASSERT_TRUE(std::isfinite(coarseRes.standardError[0]));
    ASSERT_GT(coarseRes.standardError[0], 0.0);
    const Real coarseGap = std::fabs(coarseRes.mean[0] - callRef);
    std::cout << "[ mc ] d. call surface vol ONE step: mean=" << coarseRes.mean[0]
              << " reference=" << callRef << " se=" << coarseRes.standardError[0]
              << " gap/se=" << coarseGap / coarseRes.standardError[0] << std::endl;
    EXPECT_GT(coarseGap, 3.0 * coarseRes.standardError[0])
        << "one step on a surface should be visibly wrong (local vol frozen at the spot)";
}

// e. Quanto asset: drift r_local - q - rho sigma_S sigma_FX in the note measure.
TEST(McCoreTest, QuantoAssetDriftsAtLocalRateMinusQuantoAdjustment) {
    McMarket m;
    const Real S = 100.0, q = 0.01, rLocal = 0.03, rNote = 0.05, sigmaS = 0.25, sigmaFx = 0.12;
    const Real fxSpot = 0.9, T = 1.0;
    const Date expiry = m.plusDays(365);
    const auto dividend = m.flat(q);
    const auto localRiskFree = m.flat(rLocal);
    const auto noteRiskFree = m.flat(rNote);
    const auto equityVol = m.constVol(sigmaS);
    const auto fxVol = m.constVol(sigmaFx);
    const auto grid = mc::BuildSimulationGrid(m.valuation, {expiry}, std::nullopt);
    const Size idx = grid.indexOf(expiry);
    const mc::PathFunctor level = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
        acc[0] = path[0][idx];
    };

    for (const Real rho : {-0.5, 0.7}) {
        const auto quantoDividend = buildQuantoDividendCurve(
            dividend, localRiskFree, noteRiskFree, equityVol, fxVol, fxSpot, rho, S);
        const auto asset =
            mc::MakeBasketAsset("SMI", m.spot(S), quantoDividend, noteRiskFree, equityVol);
        EXPECT_TRUE(asset.exactStepCapable);
        const auto processes = mc::BasketModel::build({asset}, identity(1));
        const Real expected = S * std::exp((rLocal - q - rho * sigmaS * sigmaFx) * T);
        const auto res =
            mc::RunPaths(processes, grid.grid, config(kPaths, kPseudo, false, false), 1, level);
        expectWithinSE(
            "e. quanto E[S_T] rho=" + std::to_string(rho), res.mean[0], res.standardError[0],
            expected, 3.0);
    }
}

// f. Determinism, variance reduction, convergence diagnostic.
TEST(McCoreTest, SeedDeterminismVarianceReductionAndRunningMeans) {
    QuantLib::SavedSettings backup;
    McMarket m;
    const Real S = 100.0, K = 100.0, r = 0.05, q = 0.02, sigma = 0.20;
    const Date expiry = m.plusDays(365);
    const auto spot = m.spot(S);
    const auto dividend = m.flat(q);
    const auto riskFree = m.flat(r);
    const auto vol = m.constVol(sigma);
    const auto processes = mc::BasketModel::build(
        {mc::MakeBasketAsset("SPX", spot, dividend, riskFree, vol)}, identity(1));
    const auto grid = mc::BuildSimulationGrid(m.valuation, {expiry}, std::nullopt);
    const Real df = riskFree->discount(1.0);
    const Real callRef = analyticEuropean(
        m, std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, K), expiry, spot,
        dividend, riskFree, vol);
    const mc::PathFunctor payoff = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
        acc[0] = df * std::max(path[0].back() - K, 0.0);
    };
    auto run = [&](const mc::PathEngineConfig& cfg) {
        return mc::RunPaths(processes, grid.grid, cfg, 1, payoff);
    };

    // Same Mersenne-Twister seed twice: identical; another seed: different.
    const auto pr42a = run(config(kPaths, kPseudo, false, false, 42));
    const auto pr42b = run(config(kPaths, kPseudo, false, false, 42));
    const auto pr43 = run(config(kPaths, kPseudo, false, false, 43));
    EXPECT_EQ(pr42a.mean[0], pr42b.mean[0]);
    EXPECT_EQ(pr42a.standardError[0], pr42b.standardError[0]);
    EXPECT_NE(pr42a.mean[0], pr43.mean[0]);

    // Sobol: the seed only affects dimensions beyond the tabulated direction
    // integers, so at this dimension every seed gives the same stream.
    const auto sobol1 = run(config(kPaths, kSobol, false, true, 1));
    const auto sobol2 = run(config(kPaths, kSobol, false, true, 2));
    EXPECT_EQ(sobol1.mean[0], sobol2.mean[0]);

    // Antithetic pairs on a monotone payoff: smaller standard error at the
    // same number of simulated paths (half the samples).
    const auto antithetic = run(config(kPaths, kPseudo, true, false, 42));
    EXPECT_EQ(antithetic.paths, kPaths);
    EXPECT_EQ(antithetic.samples, kPaths / 2);
    ASSERT_GT(antithetic.standardError[0], 0.0);
    ASSERT_GT(pr42a.standardError[0], 0.0);
    std::cout << "[ mc ] f. SE plain=" << pr42a.standardError[0]
              << " antithetic=" << antithetic.standardError[0] << std::endl;
    EXPECT_LE(antithetic.standardError[0], pr42a.standardError[0]);
    expectWithinSE(
        "f. call antithetic", antithetic.mean[0], antithetic.standardError[0], callRef, 3.0);

    // Sobol vs pseudo-random at 2^14 paths: the reported standard error is
    // the same sample statistic for both (the payoff's standard deviation /
    // sqrt(N)); the low-discrepancy gain shows in the actual error.
    const Real sobolError = std::fabs(sobol1.mean[0] - callRef);
    const Real prError = std::fabs(pr42a.mean[0] - callRef);
    std::cout << "[ mc ] f. actual error sobol=" << sobolError << " ("
              << sobolError / sobol1.standardError[0] << " SE) pseudo-random=" << prError << " ("
              << prError / pr42a.standardError[0] << " SE); SE sobol=" << sobol1.standardError[0]
              << " pseudo-random=" << pr42a.standardError[0] << std::endl;
    EXPECT_LT(sobolError, prError);

    // Running means at 1/4, 1/2, 3/4 of the samples, each within 4 SE of
    // the final mean once the SE is scaled to its own sample count.
    ASSERT_EQ(pr42a.runningMeans.size(), 3u);
    const std::vector<double> fractions = {0.25, 0.5, 0.75};
    for (Size k = 0; k < 3; ++k) {
        const auto& rm = pr42a.runningMeans[k];
        EXPECT_DOUBLE_EQ(rm.fraction, fractions[k]);
        EXPECT_EQ(rm.samples, Size(kPaths * fractions[k]));
        ASSERT_EQ(rm.means.size(), 1u);
        const Real scaledSE = pr42a.standardError[0] * std::sqrt(Real(kPaths) / Real(rm.samples));
        std::cout << "[ mc ] f. running mean at " << rm.samples << " samples: " << rm.means[0]
                  << " final=" << pr42a.mean[0]
                  << " gap/scaledSE=" << std::fabs(rm.means[0] - pr42a.mean[0]) / scaledSE
                  << std::endl;
        EXPECT_LE(std::fabs(rm.means[0] - pr42a.mean[0]), 4.0 * scaledSE);
    }
}

// g. Sample paths carry [path][asset][grid point].
TEST(McCoreTest, SamplePathsHaveAssetAndTimeDimensions) {
    McMarket m;
    const auto riskFree = m.flat(0.03);
    const auto a1 =
        mc::MakeBasketAsset("A1", m.spot(100.0), m.flat(0.0), riskFree, m.constVol(0.2));
    const auto a2 =
        mc::MakeBasketAsset("A2", m.spot(50.0), m.flat(0.01), riskFree, m.constVol(0.3));
    const auto processes = mc::BasketModel::build({a1, a2}, correlation2(0.3));
    const auto grid = mc::BuildSimulationGrid(
        m.valuation, {m.plusDays(91), m.plusDays(182), m.plusDays(365)}, Size(4));
    ASSERT_EQ(grid.size(), 6u);

    auto cfg = config(64, kSobol, true, true);
    cfg.samplePaths = 3;
    const mc::PathFunctor noop = [](const QuantLib::MultiPath&, std::vector<Real>& acc) {
        acc[0] = 1.0;
    };
    const auto res = mc::RunPaths(processes, grid.grid, cfg, 1, noop);
    ASSERT_EQ(res.samplePaths.size(), 3u);
    for (const auto& path : res.samplePaths) {
        ASSERT_EQ(path.size(), 2u);
        for (Size j = 0; j < 2; ++j) {
            ASSERT_EQ(path[j].size(), grid.size());
            EXPECT_DOUBLE_EQ(path[j][0], j == 0 ? 100.0 : 50.0);
            for (Size t = 1; t < grid.size(); ++t) {
                EXPECT_GT(path[j][t], 0.0);
            }
        }
    }
    // The second sample path is the antithetic of the first: log-returns negated.
    for (Size j = 0; j < 2; ++j) {
        const Real up = std::log(res.samplePaths[0][j][1] / res.samplePaths[0][j][0]);
        const Real down = std::log(res.samplePaths[1][j][1] / res.samplePaths[1][j][0]);
        const Real dt = grid.grid[1];
        const Real sigma = j == 0 ? 0.2 : 0.3;
        const Real q = j == 0 ? 0.0 : 0.01;
        const Real drift = (0.03 - q - 0.5 * sigma * sigma) * dt;
        EXPECT_NEAR(up - drift, -(down - drift), 1e-10);
    }
    EXPECT_EQ(res.mean[0], 1.0);
    EXPECT_EQ(res.standardError[0], 0.0);
}

// i. Exact-step table vs QuantLib's MultiPathGenerator on the quanto
//    autocall configuration (examples/data/structured_note/
//    bnp_quad_index_classic_autocall_xs2741509801.json rebuilt in QuantLib:
//    the same curve classes the parsers instantiate).
namespace {

struct QuantoBasketMarket {
    Date asOf{21, QuantLib::October, 2026};
    Handle<QuantLib::YieldTermStructure> usd;
    std::vector<mc::BasketAsset> assets;
    QuantLib::Matrix correlation{4, 4};
    std::vector<Date> observation;

    static Handle<QuantLib::YieldTermStructure> flatZero(const Date& asOf, double z) {
        std::vector<Date> d{asOf, asOf + 10 * QuantLib::Years};
        std::vector<QuantLib::Rate> r{z, z};
        return Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::ZeroCurve>(d, r, QuantLib::Actual365Fixed()));
    }
    static Handle<QuantLib::BlackVolTermStructure> constVol(
        const Date& asOf, const QuantLib::Calendar& cal, double v) {
        return Handle<QuantLib::BlackVolTermStructure>(
            std::make_shared<QuantLib::BlackConstantVol>(asOf, cal, v, QuantLib::Actual365Fixed()));
    }
    static Handle<QuantLib::Quote> spot(double v) {
        return Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(v));
    }

    QuantoBasketMarket() {
        using namespace QuantLib;
        Settings::instance().evaluationDate() = asOf;
        // USD SOFR OIS: settlement 2, payment lag 2, annual, ModifiedFollowing,
        // Discount / LogLinear on Actual/365 (Fixed).
        auto sofr = std::make_shared<Sofr>();
        struct P {
            Period tenor;
            double rate;
        };
        const std::vector<P> pts{{1 * Months, 0.0533}, {3 * Months, 0.053}, {6 * Months, 0.052},
                                 {1 * Years, 0.0495},  {2 * Years, 0.044},  {3 * Years, 0.0415},
                                 {5 * Years, 0.04},    {7 * Years, 0.0398}, {10 * Years, 0.04},
                                 {12 * Years, 0.0402}};
        std::vector<std::shared_ptr<RateHelper>> helpers;
        for (const auto& p : pts) {
            helpers.push_back(
                std::make_shared<OISRateHelper>(
                    2, p.tenor, Handle<Quote>(std::make_shared<SimpleQuote>(p.rate)), sofr,
                    Handle<YieldTermStructure>(), false, 2, ModifiedFollowing, Annual));
        }
        auto curve = std::make_shared<PiecewiseYieldCurve<Discount, LogLinear>>(
            asOf, helpers, Actual365Fixed());
        curve->enableExtrapolation();
        usd = Handle<YieldTermStructure>(curve);

        const Calendar chf = Switzerland(), nyse = UnitedStates(UnitedStates::NYSE), eur = TARGET(),
                       jpy = Japan();
        const auto smiDiv = flatZero(asOf, 0.03), chfFlat = flatZero(asOf, 0.0);
        const auto spxDiv = flatZero(asOf, 0.02);
        const auto sx5eDiv = flatZero(asOf, 0.03), eurFlat = flatZero(asOf, 0.02);
        const auto nkyDiv = flatZero(asOf, 0.02), jpyFlat = flatZero(asOf, 0.01);
        const auto smiVol = constVol(asOf, chf, 0.15), spxVol = constVol(asOf, nyse, 0.20),
                   sx5eVol = constVol(asOf, eur, 0.20), nkyVol = constVol(asOf, jpy, 0.25);
        const auto chfusdVol = constVol(asOf, chf, 0.08), eurusdVol = constVol(asOf, eur, 0.08),
                   jpyusdVol = constVol(asOf, jpy, 0.10);
        const auto smiQ =
            buildQuantoDividendCurve(smiDiv, chfFlat, usd, smiVol, chfusdVol, 1.25, -0.1, 12000.0);
        const auto sx5eQ =
            buildQuantoDividendCurve(sx5eDiv, eurFlat, usd, sx5eVol, eurusdVol, 1.1, 0.2, 5000.0);
        const auto nkyQ =
            buildQuantoDividendCurve(nkyDiv, jpyFlat, usd, nkyVol, jpyusdVol, 0.0065, 0.3, 40000.0);
        assets.push_back(mc::MakeBasketAsset("SMI", spot(12000.0), smiQ, usd, smiVol));
        assets.push_back(mc::MakeBasketAsset("SPX", spot(6000.0), spxDiv, usd, spxVol));
        assets.push_back(mc::MakeBasketAsset("SX5E", spot(5000.0), sx5eQ, usd, sx5eVol));
        assets.push_back(mc::MakeBasketAsset("NKY", spot(40000.0), nkyQ, usd, nkyVol));

        const double c[16] = {1.0, 0.6, 0.8, 0.5, 0.6, 1.0, 0.7, 0.5,
                              0.8, 0.7, 1.0, 0.5, 0.5, 0.5, 0.5, 1.0};
        for (Size i = 0; i < 4; ++i)
            for (Size j = 0; j < 4; ++j)
                correlation[i][j] = c[4 * i + j];

        observation = {Date(28, December, 2026), Date(28, June, 2027),     Date(27, December, 2027),
                       Date(27, June, 2028),     Date(27, December, 2028), Date(27, June, 2029),
                       Date(27, December, 2029), Date(27, June, 2030),     Date(27, December, 2030),
                       Date(27, June, 2031)};
    }

    /// A plain StochasticProcessArray over the same handles: RunPaths takes
    /// QuantLib's MultiPathGenerator path for it.
    std::shared_ptr<QuantLib::StochasticProcessArray> quantLibProcesses() const {
        std::vector<std::shared_ptr<QuantLib::StochasticProcess1D>> processes;
        for (const auto& a : assets) {
            processes.push_back(
                std::make_shared<QuantLib::GeneralizedBlackScholesProcess>(
                    a.spot, a.dividend, a.riskFree, a.vol));
        }
        return std::make_shared<QuantLib::StochasticProcessArray>(processes, correlation);
    }
};

} // namespace

TEST(McCoreTest, ExactStepTableReproducesQuantLibPathsBitForBit) {
    const QuantoBasketMarket m;
    const auto grid = mc::BuildSimulationGrid(m.asOf, m.observation, std::nullopt);
    ASSERT_EQ(grid.size(), 11u);

    const auto basket = mc::BasketModel::build(m.assets, m.correlation);
    ASSERT_TRUE(basket->exactStep());
    const auto quantLib = m.quantLibProcesses();

    // Worst-of autocall walk: enough state to make a one-bit difference in any
    // level visible in the means (indicators flip), plus the levels themselves.
    const Size nAcc = 3;
    const mc::PathFunctor worstOf = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
        for (Size j = 1; j < path.pathSize(); ++j) {
            Real perf = path[0][j] / 12000.0;
            perf = std::min(perf, path[1][j] / 6000.0);
            perf = std::min(perf, path[2][j] / 5000.0);
            perf = std::min(perf, path[3][j] / 40000.0);
            if (perf >= 1.0) {
                acc[0] += 1.0;
                acc[2] += grid.grid[j];
                return;
            }
            if (j + 1 == path.pathSize()) acc[1] += perf < 0.6 ? 1.0 - perf : 0.0;
        }
    };

    struct Case {
        const char* name;
        mc::PathEngineConfig::Sequence sequence;
        bool antithetic;
        bool bridge;
        Size paths;
    };
    const std::vector<Case> cases{
        {"Sobol+bridge antithetic", kSobol, true, true, 4096},
        {"Sobol antithetic", kSobol, true, false, 1024},
        {"Sobol+bridge plain", kSobol, false, true, 1024},
        {"MersenneTwister antithetic", kPseudo, true, false, 1024},
    };
    for (const auto& c : cases) {
        auto cfg = config(c.paths, c.sequence, c.antithetic, c.bridge);
        cfg.samplePaths = c.paths;
        const auto table = mc::RunPaths(basket, grid.grid, cfg, nAcc, worstOf);
        const auto reference = mc::RunPaths(quantLib, grid.grid, cfg, nAcc, worstOf);

        ASSERT_EQ(table.samplePaths.size(), c.paths) << c.name;
        ASSERT_EQ(reference.samplePaths.size(), c.paths) << c.name;
        Size levels = 0, identical = 0;
        for (Size p = 0; p < c.paths; ++p) {
            for (Size k = 0; k < 4; ++k) {
                ASSERT_EQ(table.samplePaths[p][k].size(), grid.size()) << c.name;
                for (Size j = 0; j < grid.size(); ++j) {
                    ++levels;
                    if (table.samplePaths[p][k][j] == reference.samplePaths[p][k][j]) ++identical;
                }
            }
        }
        std::cout << "[ mc ] exact-step table vs MultiPathGenerator, " << c.name << ": "
                  << identical << "/" << levels << " levels bit-identical" << std::endl;
        EXPECT_EQ(identical, levels) << c.name;
        for (Size k = 0; k < nAcc; ++k) {
            EXPECT_EQ(table.mean[k], reference.mean[k]) << c.name << " accumulator " << k;
            EXPECT_EQ(table.standardError[k], reference.standardError[k])
                << c.name << " accumulator " << k;
        }
        ASSERT_GT(table.mean[0], 0.0) << c.name << ": no path ever called, walk is degenerate";
    }
}

// h. Time grid.
// The shared, edge-clamped local vol of LocalVolCache: inside the strike
// grid every level of every path is bit-identical to QuantLib's lazily
// derived LocalVolSurface (the same object, the same Euler step); beyond
// the grid the lazy one fails on the extrapolation kink (constant strike
// extrapolation makes the total variance concave at the edge of a skewed
// wing, the Dupire denominator turns negative) while the clamped one holds
// the edge local vol. One LocalVolSurface per distinct market; an
// exact-step asset gets none.
TEST(McCoreTest, SharedLocalVolIsBitIdenticalInsideTheGridAndHoldsTheEdgeOutside) {
    McMarket m;
    const Real S = 100.0, r = 0.03, q = 0.01;
    const std::vector<Date> dates = {m.plusDays(182), m.plusDays(365), m.plusDays(730)};
    auto makeSurface = [&](const std::vector<Real>& strikes, const std::vector<Real>& volsAtK) {
        QuantLib::Matrix vols(strikes.size(), dates.size());
        for (Size i = 0; i < strikes.size(); ++i)
            for (Size j = 0; j < dates.size(); ++j)
                vols[i][j] = volsAtK[i];
        auto surface = std::make_shared<QuantLib::BlackVarianceSurface>(
            m.valuation, m.cal, dates, strikes, vols, m.dc,
            QuantLib::BlackVarianceSurface::ConstantExtrapolation,
            QuantLib::BlackVarianceSurface::ConstantExtrapolation);
        surface->setInterpolation<QuantLib::Bilinear>();
        surface->enableExtrapolation();
        return Handle<QuantLib::BlackVolTermStructure>(surface);
    };

    // a. A wide, mild surface: no path leaves the grid at 1y / 20 % vol, so
    //    the shared local vol must reproduce the lazy paths bit for bit.
    {
        const auto wide = makeSurface({20, 60, 100, 140, 180}, {0.26, 0.23, 0.20, 0.19, 0.18});
        const auto lazy = mc::MakeBasketAsset("A", m.spot(S), m.flat(q), m.flat(r), wide);
        auto shared = lazy;
        mc::LocalVolCache cache;
        cache.attach(shared);
        ASSERT_FALSE(shared.localVol.empty());
        EXPECT_TRUE(lazy.localVol.empty());
        EXPECT_EQ(cache.size(), 1u);
        auto again = lazy;
        cache.attach(again);
        EXPECT_EQ(cache.size(), 1u);
        EXPECT_EQ(again.localVol.currentLink().get(), shared.localVol.currentLink().get());
        const auto* clamped =
            dynamic_cast<const mc::EdgeClampedLocalVol*>(shared.localVol.currentLink().get());
        ASSERT_NE(clamped, nullptr);
        EXPECT_EQ(
            clamped->clampedLevel(10.0), 20.0 * std::exp(mc::EdgeClampedLocalVol::kEdgeMargin));
        EXPECT_EQ(clamped->clampedLevel(100.0), 100.0);
        EXPECT_EQ(
            clamped->clampedLevel(500.0), 180.0 * std::exp(-mc::EdgeClampedLocalVol::kEdgeMargin));

        const auto grid = mc::BuildSimulationGrid(m.valuation, {m.plusDays(365)}, Size(52));
        auto cfg = config(1024, kSobol, true, true);
        cfg.samplePaths = 16;
        const mc::PathFunctor terminal = [&](const QuantLib::MultiPath& path,
                                             std::vector<Real>& acc) { acc[0] = path[0].back(); };
        const auto a =
            mc::RunPaths(mc::BasketModel::build({lazy}, identity(1)), grid.grid, cfg, 1, terminal);
        const auto b = mc::RunPaths(
            mc::BasketModel::build({shared}, identity(1)), grid.grid, cfg, 1, terminal);
        EXPECT_EQ(a.mean[0], b.mean[0]);
        EXPECT_EQ(a.standardError[0], b.standardError[0]);
        ASSERT_EQ(a.samplePaths.size(), b.samplePaths.size());
        Size compared = 0;
        for (Size p = 0; p < a.samplePaths.size(); ++p) {
            for (Size j = 0; j < a.samplePaths[p][0].size(); ++j) {
                EXPECT_EQ(a.samplePaths[p][0][j], b.samplePaths[p][0][j])
                    << "path " << p << " point " << j;
                ++compared;
            }
        }
        EXPECT_EQ(compared, 16u * 53u);
        std::cout << "  shared local vol: " << compared << " levels bit-identical, mean "
                  << b.mean[0] << std::endl;
    }

    // b. A narrow, skewed grid (80..120, 45 % -> 36 %; a steeper skew would
    //    be butterfly-arbitrageable inside the first cell, where the
    //    piecewise-linear variance is concave in log-strike) at ~45 % vol
    //    over one year: paths reach 50 and 160. QuantLib's own
    //    LocalVolSurface fails on a level just inside the lower edge (its
    //    stencil straddles the extrapolation kink of the put wing: a concave
    //    corner, negative local variance); the clamped one evaluates there
    //    and beyond, the paths price, and a level beyond the grid reads the
    //    edge local vol.
    {
        const auto narrow = makeSurface({80, 90, 100, 110, 120}, {0.45, 0.42, 0.40, 0.38, 0.36});
        const auto lazy = mc::MakeBasketAsset("B", m.spot(S), m.flat(q), m.flat(r), narrow);
        auto shared = lazy;
        mc::LocalVolCache cache;
        cache.attach(shared);
        QuantLib::LocalVolSurface lazyLocalVol(narrow, lazy.riskFree, lazy.dividend, lazy.spot);
        EXPECT_THROW(lazyLocalVol.localVol(0.5, 80.0005, true), std::exception);
        EXPECT_NO_THROW(shared.localVol->localVol(0.5, 80.0005, true));
        EXPECT_NO_THROW(shared.localVol->localVol(0.5, 40.0, true));
        const auto grid = mc::BuildSimulationGrid(m.valuation, {m.plusDays(365)}, Size(52));
        const auto cfg = config(2048, kSobol, true, true);
        Real lo = S, hi = S;
        const mc::PathFunctor range = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
            acc[0] = path[0].back();
            lo = std::min(lo, path[0].back());
            hi = std::max(hi, path[0].back());
        };
        const auto res =
            mc::RunPaths(mc::BasketModel::build({shared}, identity(1)), grid.grid, cfg, 1, range);
        EXPECT_TRUE(std::isfinite(res.mean[0]));
        EXPECT_LT(lo, 50.0);
        EXPECT_GT(hi, 160.0);
        const QuantLib::LocalVolTermStructure& lv = *shared.localVol.currentLink();
        EXPECT_EQ(lv.localVol(0.5, 40.0, true), lv.localVol(0.5, 80.0 * std::exp(1.0e-3), true));
        EXPECT_EQ(lv.localVol(0.5, 170.0, true), lv.localVol(0.5, 120.0 * std::exp(-1.0e-3), true));
        std::cout << "  narrow grid: paths spanned [" << lo << ", " << hi << "], mean "
                  << res.mean[0] << std::endl;
    }

    // c. An exact-step asset gets no shared local vol; wiring one is a server error.
    {
        auto asset = mc::MakeBasketAsset("C", m.spot(S), m.flat(q), m.flat(r), m.constVol(0.2));
        mc::LocalVolCache cache;
        cache.attach(asset);
        EXPECT_TRUE(asset.localVol.empty());
        EXPECT_EQ(cache.size(), 0u);
    }
}

TEST(McCoreTest, TimeGridMandatoryOnlyAndRefined) {
    McMarket m;
    const Date d1 = m.plusDays(182), d2 = m.plusDays(365);

    const auto plain = mc::BuildSimulationGrid(m.valuation, {d1, d2, d2}, std::nullopt);
    ASSERT_EQ(plain.size(), 3u);
    EXPECT_DOUBLE_EQ(plain.grid[0], 0.0);
    EXPECT_DOUBLE_EQ(plain.grid[1], 182.0 / 365.0);
    EXPECT_DOUBLE_EQ(plain.grid[2], 1.0);
    EXPECT_EQ(plain.indexOf(d1), 1u);
    EXPECT_EQ(plain.indexOf(d2), 2u);
    ASSERT_EQ(plain.mandatoryIndices.size(), 3u);
    EXPECT_EQ(plain.mandatoryIndices[2], 2u);
    expectInvalidArgument(
        [&] { plain.indexOf(m.plusDays(200)); }, {"is not a mandatory date of the grid"});

    const auto withValuation =
        mc::BuildSimulationGrid(m.valuation, {m.valuation, d2}, std::nullopt);
    ASSERT_EQ(withValuation.size(), 2u);
    EXPECT_EQ(withValuation.indexOf(m.valuation), 0u);
    EXPECT_EQ(withValuation.indexOf(d2), 1u);

    // Refined: ceil(0.4986 * 12) = 6 and ceil(0.5014 * 12) = 7 steps.
    const auto monthly = mc::BuildSimulationGrid(m.valuation, {d1, d2}, Size(12));
    ASSERT_EQ(monthly.size(), 14u);
    EXPECT_EQ(monthly.indexOf(d1), 6u);
    EXPECT_EQ(monthly.indexOf(d2), 13u);
    for (Size i = 1; i < monthly.size(); ++i) {
        EXPECT_LE(monthly.grid[i] - monthly.grid[i - 1], 1.0 / 12.0 + 1e-12);
        EXPECT_GT(monthly.grid[i], monthly.grid[i - 1]);
    }
    EXPECT_DOUBLE_EQ(monthly.grid[6], 182.0 / 365.0);
    EXPECT_DOUBLE_EQ(monthly.grid[13], 1.0);

    // 110 days at 4 steps/year: 1.2 steps -> 2 steps of 0.15 (QuantLib's
    // nearest-integer rule would give one step of 0.30 > 0.25).
    const auto quarterly = mc::BuildSimulationGrid(m.valuation, {m.plusDays(110)}, Size(4));
    ASSERT_EQ(quarterly.size(), 3u);
    EXPECT_LE(quarterly.grid[1] - quarterly.grid[0], 0.25);
    EXPECT_LE(quarterly.grid[2] - quarterly.grid[1], 0.25);

    expectInvalidArgument(
        [&] { mc::BuildSimulationGrid(m.valuation, {}, std::nullopt); },
        {"at least one mandatory date is required"});
    expectInvalidArgument(
        [&] { mc::BuildSimulationGrid(m.valuation, {d2, d1}, std::nullopt); },
        {"mandatory dates must be sorted"});
    expectInvalidArgument(
        [&] { mc::BuildSimulationGrid(m.valuation, {m.valuation - 1}, std::nullopt); },
        {"is before the valuation date"});
    expectInvalidArgument(
        [&] { mc::BuildSimulationGrid(m.valuation, {m.valuation}, std::nullopt); },
        {"at least one mandatory date after the valuation date"});
    expectInvalidArgument(
        [&] { mc::BuildSimulationGrid(m.valuation, {d2}, Size(0)); },
        {"steps_per_year must be > 0"});
}

// 400 contract of the basket model and the path engine.
TEST(McCoreTest, RejectsBadInputs) {
    McMarket m;
    const auto riskFree = m.flat(0.03);
    const auto a1 =
        mc::MakeBasketAsset("A1", m.spot(100.0), m.flat(0.0), riskFree, m.constVol(0.2));
    const auto a2 = mc::MakeBasketAsset("A2", m.spot(50.0), m.flat(0.0), riskFree, m.constVol(0.3));

    expectInvalidArgument(
        [&] { mc::BasketModel::build({a1, a2}, identity(3)); },
        {"correlation matrix dimension 3 does not match the number of assets 2"});
    expectInvalidArgument(
        [&] { mc::BasketModel::build({a1, a2}, QuantLib::Matrix(2, 3, 0.0)); },
        {"correlation matrix must be square"});
    expectInvalidArgument(
        [&] { mc::BasketModel::build({a1, a1}, identity(2)); }, {"duplicate asset id: A1"});
    expectInvalidArgument(
        [&] { mc::BasketModel::build({}, identity(0)); }, {"at least one asset is required"});
    auto zeroSpot = a1;
    zeroSpot.spot = m.spot(0.0);
    expectInvalidArgument(
        [&] { mc::BasketModel::build({zeroSpot}, identity(1)); },
        {"spot must be positive for asset: A1"});
    auto wrongFlag = a1;
    wrongFlag.exactStepCapable = false;
    EXPECT_THROW(mc::BasketModel::build({wrongFlag}, identity(1)), QuantraError);

    const auto processes = mc::BasketModel::build({a1, a2}, correlation2(0.5));
    const auto grid = mc::BuildSimulationGrid(m.valuation, {m.plusDays(365)}, std::nullopt);
    const mc::PathFunctor noop = [](const QuantLib::MultiPath&, std::vector<Real>&) {};
    expectInvalidArgument(
        [&] { mc::RunPaths(processes, grid.grid, config(0, kSobol, false, true), 1, noop); },
        {"paths must be > 0"});
    expectInvalidArgument(
        [&] { mc::RunPaths(processes, grid.grid, config(7, kSobol, true, true), 1, noop); },
        {"paths must be even with antithetic variates, got 7"});
    expectInvalidArgument(
        [&] { mc::RunPaths(processes, grid.grid, config(8, kSobol, false, true), 0, noop); },
        {"at least one accumulator is required"});
    auto tooManySamples = config(8, kSobol, false, true);
    tooManySamples.samplePaths = 9;
    expectInvalidArgument(
        [&] { mc::RunPaths(processes, grid.grid, tooManySamples, 1, noop); },
        {"sample_paths (9) must be <= paths (8)"});
    EXPECT_THROW(
        mc::RunPaths(nullptr, grid.grid, config(8, kSobol, false, true), 1, noop), QuantraError);
}

}} // namespace quantra::testing
