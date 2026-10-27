#ifndef QUANTRA_CORRELATION_CALIBRATION_EVALUATOR_H
#define QUANTRA_CORRELATION_CALIBRATION_EVALUATOR_H

/**
 * CorrelationCalibrationEvaluator — pure QuantLib core of `/calibrate-correlation`.
 *
 * A correlation matrix for a basket product from the price histories of its
 * underlyings. The engine never fetches closes: it receives them, aligns
 * them, and returns a `CorrelationMatrixSpec` ready for
 * `pricing.correlations.matrices`, with everything needed to reproduce it.
 *
 * Alignment. Every series is a list of (date, close) in strictly increasing
 * date order with close > 0. The window is [as_of - window_days, as_of]
 * (calendar days), or the explicit [start_date, end_date] (either bound may
 * be open), or unbounded. The observations used are the dates inside the
 * window that every series has (an inner join), in date order; `min_overlap`
 * is checked on that count. With `frequency = Weekly` every fifth of those
 * observations is kept, counted back from the last one (the most recent
 * close is always used). At least three observations must remain.
 *
 * Returns, for series i over the T = observations_used - 1 consecutive pairs
 * of kept observations:
 *
 *     Log:    x_it = ln(P_t / P_{t-1})
 *     Simple: x_it = P_t / P_{t-1} - 1
 *
 * Sample moments, every sum in ascending t order:
 *
 *     m_i  = (1/T) sum_t x_it
 *     y_it = x_it - m_i
 *     S_ij = (1/T) sum_t y_it y_jt            (maximum-likelihood covariance,
 *                                              the normalisation of Ledoit-Wolf)
 *     r_ij = S_ij / sqrt(S_ii S_jj)           (the sample correlation; the
 *                                              1/T against 1/(T-1) choice
 *                                              cancels in the ratio)
 *     annualised_volatility_i = sqrt(S_ii * T / (T - 1) * 252 / step)
 *                                             (unbiased variance, Act/252,
 *                                              step = 1 Daily, 5 Weekly)
 *
 * Shrinkage toward the constant-correlation target, Ledoit & Wolf (2004),
 * "Honey, I shrunk the sample covariance matrix", Journal of Portfolio
 * Management 30(4). With N series, the target F has f_ii = S_ii and
 * f_ij = rbar sqrt(S_ii S_jj), where
 *
 *     rbar = (2 / (N (N - 1))) sum_{i<j} r_ij
 *
 * and the optimal intensity estimate is
 *
 *     pi_ij      = (1/T) sum_t (y_it y_jt - S_ij)^2,          pi = sum_ij pi_ij
 *     theta_ii,ij = (1/T) sum_t (y_it^2 - S_ii) (y_it y_jt - S_ij)
 *     rho        = sum_i pi_ii
 *                + sum_{i != j} (rbar / 2) ( sqrt(S_jj / S_ii) theta_ii,ij
 *                                           + sqrt(S_ii / S_jj) theta_jj,ij )
 *     gamma      = sum_ij (f_ij - S_ij)^2
 *     kappa      = (pi - rho) / gamma
 *     delta      = max(0, min(1, kappa / T))
 *
 * The shrunk covariance delta F + (1 - delta) S has the diagonal of S, so in
 * correlation terms the result is
 *
 *     c_ij = delta rbar + (1 - delta) r_ij     (i != j),   c_ii = 1.
 *
 * With exactly two series the target is the sample correlation itself
 * (gamma = 0 up to rounding), so delta is reported as 0: there is nothing to
 * shrink. For N >= 3 a gamma that is not > 0 (every pair at the same
 * correlation) also gives delta = 0. `Fixed` uses the request's intensity;
 * `None` uses 0.
 *
 * Eigenvalue floor. The smallest eigenvalue of C (QuantLib
 * SymmetricSchurDecomposition, Jacobi) is reported; when it is below -1e-10
 * (the correlation registry's own acceptance rule: a rank-deficient matrix
 * such as a perfectly correlated pair, eigenvalue 0, is accepted as is) the
 * matrix is repaired as V diag(max(lambda, eigenvalue_floor)) V^T rescaled to
 * unit diagonal (the structured note's correlation bump repair with the
 * floor in place of 0) and the smallest eigenvalue of the result is reported
 * next to it. A sample correlation is positive semi-definite in exact
 * arithmetic, and so is the constant-correlation target (the average
 * correlation of a positive semi-definite matrix is >= -1/(N-1)) and every
 * convex combination of the two; the floor meets rounding only, on a
 * singular set (a series that is a combination of the others, more series
 * than returns), and guarantees that the returned spec is a valid pricing
 * input.
 *
 * Every output is deterministic: the inputs are read in the order given,
 * every sum runs in the stated order, and the Python reference
 * `calibrate_correlation_ql` (tests/contract/ql_reference.py) repeats the
 * arithmetic step by step so the two agree to 1e-12.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * correlation_calibration_mapper.{h,cpp}.
 */

#include "pricing_context.h"
#include "pricing_registry.h"

#include <ql/math/matrix.hpp>
#include <ql/time/date.hpp>

#include <optional>
#include <string>
#include <vector>

namespace quantra {

enum class ReturnKind { Log, Simple };
enum class ReturnSampling { Daily, Weekly };
enum class ShrinkageKind { None, LedoitWolf, Fixed };

struct PriceObservation {
    QuantLib::Date date;
    std::string dateIso;
    double value = 0.0;
};

struct PriceSeriesData {
    std::string underlyingId;
    std::vector<PriceObservation> closes;
};

/// Estimator settings (request values or the schema defaults). The result
/// carries the same struct as resolved: the window as explicit dates, no
/// `windowDays`, `shrinkageIntensity` present iff `Fixed`.
struct CorrelationEstimatorData {
    std::optional<unsigned> windowDays;
    std::optional<QuantLib::Date> startDate;
    std::optional<QuantLib::Date> endDate;
    ReturnKind returns = ReturnKind::Log;
    ReturnSampling frequency = ReturnSampling::Daily;
    ShrinkageKind shrinkage = ShrinkageKind::LedoitWolf;
    std::optional<double> shrinkageIntensity;
    unsigned minOverlap = 60;
    double eigenvalueFloor = 1e-8;
};

struct CorrelationCalibrationInputs {
    QuantLib::Date asOf;
    std::vector<PriceSeriesData> series;
    CorrelationEstimatorData estimator;
    std::string correlationId;
    /// Output order; empty = series order.
    std::vector<std::string> labels;
};

struct CorrelationSeriesReportData {
    std::string underlyingId;
    unsigned observations = 0;
    unsigned dropped = 0;
    double annualisedVolatility = 0.0;
};

struct CorrelationCalibrationResult {
    std::string correlationId;
    /// Output label order; both matrices are row-major in this order.
    std::vector<std::string> labels;
    std::vector<double> correlation;
    std::vector<double> sampleCorrelation;
    double shrinkageTarget = 0.0;
    double shrinkageIntensityUsed = 0.0;
    bool eigenvalueFloorApplied = false;
    double smallestEigenvalueBefore = 0.0;
    double smallestEigenvalueAfter = 0.0;
    unsigned observationsUsed = 0;
    unsigned returnsUsed = 0;
    std::string firstDateIso;
    std::string lastDateIso;
    std::vector<CorrelationSeriesReportData> series;
    CorrelationEstimatorData estimatorEcho;
};

/// Sample moments of demeaned returns `y[i][t]` (N series, T returns): the
/// 1/T covariance and the correlation (see the header). The matrices are
/// N x N.
struct SampleMoments {
    QuantLib::Matrix covariance;
    QuantLib::Matrix correlation;
};
SampleMoments sampleMoments(const std::vector<std::vector<double>>& y);

/// The Ledoit-Wolf intensity toward the constant-correlation target for
/// demeaned returns `y[i][t]` with moments `m`: (rbar, delta) with delta
/// clipped to [0, 1]; delta = 0 for two series or a non-positive gamma.
struct LedoitWolfEstimate {
    double target = 0.0;
    double intensity = 0.0;
};
LedoitWolfEstimate ledoitWolfConstantCorrelation(
    const std::vector<std::vector<double>>& y, const SampleMoments& m);

/// Smallest eigenvalue of a symmetric matrix (SymmetricSchurDecomposition).
double smallestEigenvalue(const QuantLib::Matrix& c);

/// Eigenvalue floor: `c` unchanged when its smallest eigenvalue is >= -1e-10,
/// otherwise V diag(max(lambda, floor)) V^T rescaled to unit diagonal
/// (exactly symmetric).
struct FlooredMatrix {
    QuantLib::Matrix matrix;
    bool applied = false;
    double smallestBefore = 0.0;
    double smallestAfter = 0.0;
};
FlooredMatrix floorEigenvalues(const QuantLib::Matrix& c, double floor);

class CorrelationCalibrationEvaluator {
public:
    /// `reg` and `ctx` are unused: the endpoint carries no Pricing block.
    CorrelationCalibrationResult evaluate(
        const CorrelationCalibrationInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CORRELATION_CALIBRATION_EVALUATOR_H
