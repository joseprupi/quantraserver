// Correlation from price histories: evaluator tests.
//
// The series are generated here from known inputs (fixed-seed Gaussian
// returns, a population correlation matrix through its Cholesky factor,
// exact linear combinations), so the estimate can be held against what was
// put in: two perfectly correlated series give exactly 1, independent ones
// give about 0 within the sampling error, a 3 x 3 population matrix is
// recovered within sampling error, the Ledoit-Wolf intensity lies in [0, 1]
// and moves the matrix toward the target, a fixed intensity of 1 gives the
// target, a singular set exercises the eigenvalue floor, and every 400 names
// its field.
#include "correlation_calibration_evaluator.h"
#include "error.h"

#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/randomnumbers/mt19937uniformrng.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;

const Date kStart(1, QuantLib::January, 2025);

std::string iso(const Date& d) {
    char buf[16];
    std::snprintf(
        buf, sizeof(buf), "%04d-%02d-%02d", static_cast<int>(d.year()), static_cast<int>(d.month()),
        static_cast<int>(d.dayOfMonth()));
    return buf;
}

/// A price path from log returns, one close per calendar day from kStart.
PriceSeriesData series(
    const std::string& id, const std::vector<double>& logReturns, double p0 = 100.0) {
    PriceSeriesData s;
    s.underlyingId = id;
    double p = p0;
    Date d = kStart;
    s.closes.push_back({d, iso(d), p});
    for (double r : logReturns) {
        p *= std::exp(r);
        d = d + 1;
        s.closes.push_back({d, iso(d), p});
    }
    return s;
}

/// Standard normal draws, fixed seed.
std::vector<std::vector<double>> normals(std::size_t n, std::size_t T, unsigned long seed) {
    QuantLib::MersenneTwisterUniformRng rng(seed);
    QuantLib::InverseCumulativeNormal icn;
    std::vector<std::vector<double>> z(n, std::vector<double>(T));
    for (std::size_t t = 0; t < T; ++t)
        for (std::size_t i = 0; i < n; ++i)
            z[i][t] = icn(rng.next().value);
    return z;
}

CorrelationCalibrationInputs inputs(
    std::vector<PriceSeriesData> s, ShrinkageKind shrink = ShrinkageKind::None) {
    CorrelationCalibrationInputs in;
    in.asOf = s.front().closes.back().date;
    in.series = std::move(s);
    in.correlationId = "TEST";
    in.estimator.shrinkage = shrink;
    return in;
}

CorrelationCalibrationResult run(const CorrelationCalibrationInputs& in) {
    PricingRegistry reg;
    PricingContext ctx;
    return CorrelationCalibrationEvaluator().evaluate(in, reg, ctx);
}

void expectInvalid(const CorrelationCalibrationInputs& in, const std::string& needle) {
    try {
        run(in);
        FAIL() << "expected a 400 mentioning '" << needle << "'";
    } catch (const QuantraInvalidArgument& e) {
        EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
    }
}

} // namespace

TEST(CorrelationCalibration, PerfectlyCorrelatedSeriesGiveExactlyOne) {
    const auto z = normals(1, 300, 11);
    std::vector<double> r(300);
    for (std::size_t t = 0; t < 300; ++t)
        r[t] = 0.01 * z[0][t];
    // The same log returns at a different level: B = 2 A, so the returns are
    // identical to the bit.
    auto in = inputs({series("A", r, 100.0), series("B", r, 200.0)});
    const auto out = run(in);
    ASSERT_EQ(out.labels, (std::vector<std::string>{"A", "B"}));
    EXPECT_EQ(out.sampleCorrelation[1], 1.0);
    EXPECT_EQ(out.correlation[1], 1.0);
    EXPECT_EQ(out.correlation[2], 1.0);
    EXPECT_EQ(out.correlation[0], 1.0);
    // Rank-deficient (eigenvalue 0): accepted as is, like the registry does.
    EXPECT_FALSE(out.eigenvalueFloorApplied);
    EXPECT_LT(std::fabs(out.smallestEigenvalueBefore), 1e-12);
    EXPECT_EQ(out.observationsUsed, 301u);
    EXPECT_EQ(out.returnsUsed, 300u);
    EXPECT_EQ(out.shrinkageIntensityUsed, 0.0);
    EXPECT_EQ(out.firstDateIso, "2025-01-01");
    EXPECT_EQ(out.series[0].dropped, 0u);
    EXPECT_DOUBLE_EQ(out.series[0].annualisedVolatility, out.series[1].annualisedVolatility);
    // Shrinkage with two series: the target is the sample itself.
    in.estimator.shrinkage = ShrinkageKind::LedoitWolf;
    const auto lw = run(in);
    EXPECT_EQ(lw.shrinkageIntensityUsed, 0.0);
    EXPECT_EQ(lw.shrinkageTarget, 1.0);
}

TEST(CorrelationCalibration, IndependentSeriesGiveAboutZero) {
    const std::size_t T = 2000;
    const auto z = normals(2, T, 7);
    std::vector<double> a(T), b(T);
    for (std::size_t t = 0; t < T; ++t) {
        a[t] = 0.012 * z[0][t];
        b[t] = 0.020 * z[1][t];
    }
    const auto out = run(inputs({series("A", a), series("B", b)}));
    // Standard error of a correlation estimate near 0 is 1 / sqrt(T) = 0.022.
    EXPECT_LT(std::fabs(out.sampleCorrelation[1]), 3.5 / std::sqrt(static_cast<double>(T)));
    EXPECT_NEAR(
        out.series[0].annualisedVolatility, 0.012 * std::sqrt(252.0),
        0.012 * std::sqrt(252.0) * 0.05);
    EXPECT_NEAR(
        out.series[1].annualisedVolatility, 0.020 * std::sqrt(252.0),
        0.020 * std::sqrt(252.0) * 0.05);
}

TEST(CorrelationCalibration, RecoversAThreeByThreePopulationMatrix) {
    // Population correlation through its Cholesky factor.
    const double r12 = 0.6, r13 = 0.3, r23 = 0.5;
    const double l21 = r12, l22 = std::sqrt(1.0 - r12 * r12);
    const double l31 = r13, l32 = (r23 - l21 * l31) / l22;
    const double l33 = std::sqrt(1.0 - l31 * l31 - l32 * l32);
    const std::size_t T = 4000;
    const auto z = normals(3, T, 2024);
    std::vector<double> a(T), b(T), c(T);
    for (std::size_t t = 0; t < T; ++t) {
        a[t] = 0.01 * z[0][t];
        b[t] = 0.01 * (l21 * z[0][t] + l22 * z[1][t]);
        c[t] = 0.01 * (l31 * z[0][t] + l32 * z[1][t] + l33 * z[2][t]);
    }
    auto in = inputs({series("A", a), series("B", b), series("C", c)});
    const auto out = run(in);
    // Standard error (1 - rho^2) / sqrt(T) <= 0.016; allow 4 of them.
    const double tol = 4.0 / std::sqrt(static_cast<double>(T));
    EXPECT_NEAR(out.sampleCorrelation[0 * 3 + 1], r12, tol);
    EXPECT_NEAR(out.sampleCorrelation[0 * 3 + 2], r13, tol);
    EXPECT_NEAR(out.sampleCorrelation[1 * 3 + 2], r23, tol);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(out.correlation[i * 3 + i], 1.0);
        for (std::size_t j = 0; j < 3; ++j)
            EXPECT_EQ(out.correlation[i * 3 + j], out.correlation[j * 3 + i]);
    }
    EXPECT_FALSE(out.eigenvalueFloorApplied);
    EXPECT_GT(out.smallestEigenvalueBefore, 0.1);
    EXPECT_EQ(out.smallestEigenvalueAfter, out.smallestEigenvalueBefore);

    // Ledoit-Wolf: intensity in [0, 1], target = average off-diagonal sample
    // correlation, every off-diagonal entry between the sample and the target.
    in.estimator.shrinkage = ShrinkageKind::LedoitWolf;
    const auto lw = run(in);
    const double target =
        (lw.sampleCorrelation[1] + lw.sampleCorrelation[2] + lw.sampleCorrelation[5]) / 3.0;
    EXPECT_NEAR(lw.shrinkageTarget, target, 1e-15);
    EXPECT_GE(lw.shrinkageIntensityUsed, 0.0);
    EXPECT_LE(lw.shrinkageIntensityUsed, 1.0);
    EXPECT_GT(lw.shrinkageIntensityUsed, 0.0); // a finite sample always shrinks a little
    for (std::size_t k : {1u, 2u, 5u}) {
        const double s = lw.sampleCorrelation[k], c = lw.correlation[k];
        const double expected =
            lw.shrinkageIntensityUsed * target + (1.0 - lw.shrinkageIntensityUsed) * s;
        EXPECT_NEAR(c, expected, 1e-15);
        EXPECT_LE(std::min(s, target) - 1e-15, c);
        EXPECT_GE(std::max(s, target) + 1e-15, c);
    }
    EXPECT_EQ(lw.sampleCorrelation, out.sampleCorrelation);

    // Fixed intensities: 1 gives the target matrix, 0 the sample.
    in.estimator.shrinkage = ShrinkageKind::Fixed;
    in.estimator.shrinkageIntensity = 1.0;
    const auto one = run(in);
    EXPECT_EQ(one.shrinkageIntensityUsed, 1.0);
    for (std::size_t k : {1u, 2u, 3u, 5u, 6u, 7u})
        EXPECT_NEAR(one.correlation[k], target, 1e-15);
    in.estimator.shrinkageIntensity = 0.0;
    const auto zero = run(in);
    EXPECT_EQ(zero.correlation, out.correlation);
    EXPECT_TRUE(zero.estimatorEcho.shrinkageIntensity.has_value());
    EXPECT_FALSE(lw.estimatorEcho.shrinkageIntensity.has_value());
}

TEST(CorrelationCalibration, EigenvalueFloorRepairsASingularSet) {
    // C's log returns are the sum of A's and B's: the return matrix has rank
    // 2 and the sample correlation is singular (an eigenvalue at 0 up to
    // rounding).
    const std::size_t T = 500;
    const auto z = normals(2, T, 99);
    std::vector<double> a(T), b(T), c(T);
    for (std::size_t t = 0; t < T; ++t) {
        a[t] = 0.01 * z[0][t];
        b[t] = 0.015 * z[1][t];
        c[t] = a[t] + b[t];
    }
    auto in = inputs({series("A", a), series("B", b), series("C", c)});
    in.estimator.eigenvalueFloor = 1e-6;
    const auto out = run(in);
    // An eigenvalue at 0 up to rounding: repaired iff rounding put it below
    // the registry's -1e-10, and either way a valid pricing input.
    EXPECT_LT(std::fabs(out.smallestEigenvalueBefore), 1e-10);
    EXPECT_EQ(out.eigenvalueFloorApplied, out.smallestEigenvalueBefore < -1e-10);
    EXPECT_GE(out.smallestEigenvalueAfter, -1e-10);
    for (std::size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(out.correlation[i * 3 + i], 1.0);
        for (std::size_t j = 0; j < 3; ++j) {
            EXPECT_EQ(out.correlation[i * 3 + j], out.correlation[j * 3 + i]);
            EXPECT_NEAR(out.correlation[i * 3 + j], out.sampleCorrelation[i * 3 + j], 1e-4);
        }
    }
    // The building block on its own: a matrix with a negative eigenvalue.
    QuantLib::Matrix m(3, 3, 0.9);
    m[0][0] = m[1][1] = m[2][2] = 1.0;
    m[0][2] = m[2][0] = -0.9;
    const auto fl = floorEigenvalues(m, 1e-8);
    EXPECT_TRUE(fl.applied);
    EXPECT_LT(fl.smallestBefore, -0.5);
    EXPECT_GT(fl.smallestAfter, -1e-12);
    for (QuantLib::Size i = 0; i < 3; ++i) {
        EXPECT_EQ(fl.matrix[i][i], 1.0);
        for (QuantLib::Size j = 0; j < 3; ++j) {
            EXPECT_EQ(fl.matrix[i][j], fl.matrix[j][i]);
            EXPECT_LE(std::fabs(fl.matrix[i][j]), 1.0);
        }
    }
    // A larger floor raises the clipped eigenvalue further.
    const auto fl2 = floorEigenvalues(m, 0.05);
    EXPECT_TRUE(fl2.applied);
    EXPECT_GT(fl2.smallestAfter, fl.smallestAfter);
    // Rank-deficient but positive semi-definite: left alone.
    QuantLib::Matrix ones(2, 2, 1.0);
    EXPECT_FALSE(floorEigenvalues(ones, 1e-8).applied);
    QuantLib::Matrix fine(2, 2, 0.5);
    fine[0][0] = fine[1][1] = 1.0;
    const auto ok = floorEigenvalues(fine, 1e-8);
    EXPECT_FALSE(ok.applied);
    EXPECT_NEAR(ok.smallestBefore, 0.5, 1e-12);
    EXPECT_EQ(ok.matrix[0][1], 0.5);
}

TEST(CorrelationCalibration, WindowsSamplingLabelsAndEcho) {
    const std::size_t T = 400;
    const auto z = normals(2, T, 3);
    std::vector<double> a(T), b(T);
    for (std::size_t t = 0; t < T; ++t) {
        a[t] = 0.01 * z[0][t];
        b[t] = 0.01 * (0.5 * z[0][t] + std::sqrt(0.75) * z[1][t]);
    }
    auto A = series("A", a);
    auto B = series("B", b);
    // B lacks the first 10 dates and has 5 extra ones after A's last.
    B.closes.erase(B.closes.begin(), B.closes.begin() + 10);
    for (int k = 1; k <= 5; ++k) {
        Date d = A.closes.back().date + k;
        B.closes.push_back({d, iso(d), B.closes.back().value * 1.001});
    }
    auto in = inputs({A, B});
    in.labels = {"B", "A"};
    const auto all = run(in);
    EXPECT_EQ(all.labels, (std::vector<std::string>{"B", "A"}));
    EXPECT_EQ(all.observationsUsed, T + 1 - 10);
    EXPECT_EQ(all.series[0].underlyingId, "B");
    EXPECT_EQ(all.series[0].observations, T + 1 - 10 + 5);
    EXPECT_EQ(all.series[0].dropped, 5u);
    EXPECT_EQ(all.series[1].dropped, 10u);
    EXPECT_EQ(all.firstDateIso, iso(A.closes[10].date));
    EXPECT_EQ(all.lastDateIso, iso(A.closes.back().date));
    EXPECT_EQ(all.estimatorEcho.startDate, A.closes[10].date);
    EXPECT_EQ(all.estimatorEcho.endDate, A.closes.back().date);
    EXPECT_FALSE(all.estimatorEcho.windowDays.has_value());
    // The same estimate in series order is the permuted matrix.
    in.labels.clear();
    const auto plain = run(in);
    EXPECT_EQ(plain.correlation[1], all.correlation[1]);
    EXPECT_EQ(plain.series[0].underlyingId, "A");

    // window_days from as_of (A's last date): 100 days back = 101 observations.
    in.estimator.windowDays = 100;
    const auto win = run(in);
    EXPECT_EQ(win.observationsUsed, 101u);
    EXPECT_EQ(win.lastDateIso, iso(in.asOf));
    EXPECT_EQ(win.estimatorEcho.startDate, in.asOf - 100);
    EXPECT_EQ(win.estimatorEcho.endDate, in.asOf);
    EXPECT_FALSE(win.estimatorEcho.windowDays.has_value());
    // Posting the echo back reproduces the result.
    in.estimator = win.estimatorEcho;
    const auto again = run(in);
    EXPECT_EQ(again.correlation, win.correlation);
    EXPECT_EQ(again.observationsUsed, win.observationsUsed);

    // Explicit window, and as_of before the last close is allowed.
    in.estimator = CorrelationEstimatorData{};
    in.estimator.shrinkage = ShrinkageKind::None;
    in.estimator.startDate = A.closes[50].date;
    in.estimator.endDate = A.closes[149].date;
    in.asOf = A.closes[100].date;
    const auto expl = run(in);
    EXPECT_EQ(expl.observationsUsed, 100u);
    EXPECT_EQ(expl.firstDateIso, iso(A.closes[50].date));

    // Weekly: every fifth common observation counted back from the last.
    in.estimator = CorrelationEstimatorData{};
    in.estimator.shrinkage = ShrinkageKind::None;
    in.estimator.frequency = ReturnSampling::Weekly;
    in.asOf = A.closes.back().date;
    const auto weekly = run(in);
    EXPECT_EQ(weekly.observationsUsed, (T + 1 - 10 - 1) / 5 + 1);
    EXPECT_EQ(weekly.lastDateIso, iso(A.closes.back().date));
    EXPECT_EQ(weekly.series[0].dropped, T + 1 - weekly.observationsUsed);
    // Five-day returns, annualised with 252 / 5.
    EXPECT_NEAR(
        weekly.series[0].annualisedVolatility, 0.01 * std::sqrt(252.0),
        0.01 * std::sqrt(252.0) * 0.2);

    // Simple returns differ from log returns, slightly.
    in.estimator = CorrelationEstimatorData{};
    in.estimator.shrinkage = ShrinkageKind::None;
    in.estimator.returns = ReturnKind::Simple;
    const auto simple = run(in);
    EXPECT_NE(simple.correlation[1], plain.correlation[1]);
    EXPECT_NEAR(simple.correlation[1], plain.correlation[1], 0.01);
}

TEST(CorrelationCalibration, RejectedRequestsNameTheField) {
    const auto z = normals(2, 100, 5);
    std::vector<double> a(100), b(100);
    for (std::size_t t = 0; t < 100; ++t) {
        a[t] = 0.01 * z[0][t];
        b[t] = 0.01 * z[1][t];
    }
    const auto A = series("A", a), B = series("B", b);
    auto base = inputs({A, B});

    expectInvalid(inputs({A}), "at least two price series");
    expectInvalid(inputs({A, series("A", b)}), "series[1].underlying_id: duplicate 'A'");
    {
        auto in = base;
        std::swap(in.series[1].closes[3], in.series[1].closes[4]);
        expectInvalid(in, "series[1].closes[4].date");
    }
    {
        auto in = base;
        in.series[0].closes[7].value = 0.0;
        expectInvalid(in, "series[0].closes[7].value must be finite and > 0");
    }
    {
        auto in = base;
        in.estimator.minOverlap = 200;
        expectInvalid(in, "below estimator.min_overlap (200)");
    }
    {
        auto in = base;
        in.estimator.minOverlap = 2;
        expectInvalid(in, "estimator.min_overlap must be >= 3");
    }
    {
        auto in = base;
        in.labels = {"A"};
        expectInvalid(in, "labels must be a permutation");
        in.labels = {"A", "C"};
        expectInvalid(in, "labels: 'C' is not a series id");
        in.labels = {"A", "A"};
        expectInvalid(in, "labels: 'A' listed twice");
    }
    {
        auto in = base;
        in.estimator.windowDays = 30;
        in.estimator.startDate = kStart;
        expectInvalid(in, "either window_days or start_date / end_date");
        in.estimator.startDate.reset();
        in.estimator.windowDays = 0;
        expectInvalid(in, "estimator.window_days must be > 0");
    }
    {
        auto in = base;
        in.estimator.startDate = kStart + 10;
        in.estimator.endDate = kStart + 5;
        expectInvalid(in, "estimator.start_date 2025-01-11 is after end_date 2025-01-06");
    }
    {
        auto in = base;
        in.estimator.shrinkage = ShrinkageKind::Fixed;
        expectInvalid(in, "shrinkage_intensity is required with shrinkage = Fixed");
        in.estimator.shrinkageIntensity = 1.5;
        expectInvalid(in, "shrinkage_intensity must be in [0, 1]");
        in.estimator.shrinkage = ShrinkageKind::LedoitWolf;
        in.estimator.shrinkageIntensity = 0.5;
        expectInvalid(in, "shrinkage_intensity is only read with shrinkage = Fixed");
    }
    {
        auto in = base;
        in.estimator.eigenvalueFloor = 1.0;
        expectInvalid(in, "eigenvalue_floor must be finite and in [0, 1)");
    }
    {
        // A window with no common date, and one with a few.
        auto in = base;
        in.estimator.startDate = kStart + 500;
        expectInvalid(in, "contains no date common to every series");
        in.estimator.startDate = kStart + 99;
        in.estimator.minOverlap = 3;
        expectInvalid(in, "overlap of 2 common dates");
        in.estimator.startDate = kStart + 97;
        in.estimator.frequency = ReturnSampling::Weekly;
        expectInvalid(in, "fewer than 3 observations after the Weekly sampling of 4 common dates");
    }
    {
        auto in = base;
        in.correlationId.clear();
        expectInvalid(in, "correlation_id is required");
    }
}

}} // namespace quantra::testing
