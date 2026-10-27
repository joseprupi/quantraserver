#include "correlation_calibration_evaluator.h"

#include "date_convert.h"
#include "error.h"

#include <ql/math/matrixutilities/symmetricschurdecomposition.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace quantra {

namespace {

using QuantLib::Date;
using QuantLib::Matrix;
using QuantLib::Size;

constexpr unsigned kWeeklyStep = 5;
constexpr double kTradingDaysPerYear = 252.0;
// The correlation registry accepts a matrix whose smallest eigenvalue is
// >= -1e-10 (rank-deficient sets such as a perfectly correlated pair have
// eigenvalue 0 in exact arithmetic); the floor repairs only what the
// registry would refuse.
constexpr double kPsdTolerance = 1e-10;

std::string fmt(double v) {
    std::ostringstream os;
    os.precision(15);
    os << v;
    return os.str();
}

std::string join(const std::vector<std::string>& v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (i > 0) out += ", ";
        out += v[i];
    }
    return out;
}

void validateSeries(const std::vector<PriceSeriesData>& series) {
    if (series.size() < 2) {
        QUANTRA_INVALID_ARGUMENT(
            "series: at least two price series are needed, got " + std::to_string(series.size()));
    }
    std::set<std::string> ids;
    for (std::size_t i = 0; i < series.size(); ++i) {
        const auto& s = series[i];
        const std::string p = "series[" + std::to_string(i) + "]";
        if (s.underlyingId.empty()) {
            QUANTRA_INVALID_ARGUMENT(p + ".underlying_id is required");
        }
        if (!ids.insert(s.underlyingId).second) {
            QUANTRA_INVALID_ARGUMENT(p + ".underlying_id: duplicate '" + s.underlyingId + "'");
        }
        if (s.closes.empty()) {
            QUANTRA_INVALID_ARGUMENT(p + ".closes must contain at least one close");
        }
        for (std::size_t k = 0; k < s.closes.size(); ++k) {
            const auto& c = s.closes[k];
            const std::string q = p + ".closes[" + std::to_string(k) + "]";
            if (!std::isfinite(c.value) || !(c.value > 0.0)) {
                QUANTRA_INVALID_ARGUMENT(
                    q + ".value must be finite and > 0, got " + fmt(c.value) + " (" +
                    s.underlyingId + ", " + c.dateIso + ")");
            }
            if (k > 0 && !(c.date > s.closes[k - 1].date)) {
                QUANTRA_INVALID_ARGUMENT(
                    q + ".date " + c.dateIso + " is not after the previous close " +
                    s.closes[k - 1].dateIso + " (dates must be strictly increasing, " +
                    s.underlyingId + ")");
            }
        }
    }
}

void validateEstimator(const CorrelationEstimatorData& e) {
    if (e.windowDays && (e.startDate || e.endDate)) {
        QUANTRA_INVALID_ARGUMENT(
            "estimator.window_days: give either window_days or start_date / end_date, not both");
    }
    if (e.windowDays && *e.windowDays == 0) {
        QUANTRA_INVALID_ARGUMENT("estimator.window_days must be > 0");
    }
    if (e.startDate && e.endDate && *e.startDate > *e.endDate) {
        QUANTRA_INVALID_ARGUMENT(
            "estimator.start_date " + DateToIso(*e.startDate) + " is after end_date " +
            DateToIso(*e.endDate));
    }
    if (e.shrinkage == ShrinkageKind::Fixed) {
        if (!e.shrinkageIntensity) {
            QUANTRA_INVALID_ARGUMENT(
                "estimator.shrinkage_intensity is required with shrinkage = Fixed");
        }
        if (!std::isfinite(*e.shrinkageIntensity) || *e.shrinkageIntensity < 0.0 ||
            *e.shrinkageIntensity > 1.0) {
            QUANTRA_INVALID_ARGUMENT(
                "estimator.shrinkage_intensity must be in [0, 1], got " +
                fmt(*e.shrinkageIntensity));
        }
    } else if (e.shrinkageIntensity) {
        QUANTRA_INVALID_ARGUMENT(
            "estimator.shrinkage_intensity is only read with shrinkage = Fixed (remove it, or "
            "set shrinkage to Fixed)");
    }
    if (e.minOverlap < 3) {
        QUANTRA_INVALID_ARGUMENT(
            "estimator.min_overlap must be >= 3 (two returns at least), got " +
            std::to_string(e.minOverlap));
    }
    if (!std::isfinite(e.eigenvalueFloor) || e.eigenvalueFloor < 0.0 || e.eigenvalueFloor >= 1.0) {
        QUANTRA_INVALID_ARGUMENT(
            "estimator.eigenvalue_floor must be finite and in [0, 1), got " +
            fmt(e.eigenvalueFloor));
    }
}

/// Output order: the request's labels (a permutation of the series ids) or
/// the series order. Returns the series index of each output position.
std::vector<std::size_t> outputOrder(
    const std::vector<PriceSeriesData>& series, const std::vector<std::string>& labels) {
    std::vector<std::size_t> order;
    if (labels.empty()) {
        for (std::size_t i = 0; i < series.size(); ++i)
            order.push_back(i);
        return order;
    }
    std::unordered_map<std::string, std::size_t> index;
    for (std::size_t i = 0; i < series.size(); ++i)
        index[series[i].underlyingId] = i;
    std::vector<std::string> ids;
    for (const auto& s : series)
        ids.push_back(s.underlyingId);
    if (labels.size() != series.size()) {
        QUANTRA_INVALID_ARGUMENT(
            "labels must be a permutation of the series ids [" + join(ids) + "], got " +
            std::to_string(labels.size()) + " labels");
    }
    std::set<std::string> seen;
    for (const auto& l : labels) {
        auto it = index.find(l);
        if (it == index.end()) {
            QUANTRA_INVALID_ARGUMENT(
                "labels: '" + l + "' is not a series id (series: [" + join(ids) + "])");
        }
        if (!seen.insert(l).second) {
            QUANTRA_INVALID_ARGUMENT("labels: '" + l + "' listed twice");
        }
        order.push_back(it->second);
    }
    return order;
}

} // namespace

SampleMoments sampleMoments(const std::vector<std::vector<double>>& y) {
    const Size n = y.size();
    const Size T = n ? y[0].size() : 0;
    SampleMoments m;
    m.covariance = Matrix(n, n, 0.0);
    m.correlation = Matrix(n, n, 1.0);
    for (Size i = 0; i < n; ++i) {
        for (Size j = 0; j < n; ++j) {
            double s = 0.0;
            for (Size t = 0; t < T; ++t)
                s += y[i][t] * y[j][t];
            m.covariance[i][j] = s / static_cast<double>(T);
        }
    }
    for (Size i = 0; i < n; ++i) {
        for (Size j = 0; j < n; ++j) {
            if (i == j) continue;
            m.correlation[i][j] =
                m.covariance[i][j] / std::sqrt(m.covariance[i][i] * m.covariance[j][j]);
        }
    }
    return m;
}

LedoitWolfEstimate ledoitWolfConstantCorrelation(
    const std::vector<std::vector<double>>& y, const SampleMoments& m) {
    const Size n = y.size();
    const Size T = n ? y[0].size() : 0;
    const Matrix& S = m.covariance;
    const Matrix& R = m.correlation;
    LedoitWolfEstimate out;
    double rsum = 0.0;
    for (Size i = 0; i < n; ++i)
        for (Size j = i + 1; j < n; ++j)
            rsum += R[i][j];
    out.target = rsum * 2.0 / (static_cast<double>(n) * static_cast<double>(n - 1));
    if (n == 2) return out; // the target is the sample correlation: nothing to shrink

    const double Td = static_cast<double>(T);
    double pi = 0.0, rho = 0.0, gamma = 0.0;
    for (Size i = 0; i < n; ++i) {
        for (Size j = 0; j < n; ++j) {
            double pij = 0.0;
            for (Size t = 0; t < T; ++t) {
                const double d = y[i][t] * y[j][t] - S[i][j];
                pij += d * d;
            }
            pij /= Td;
            pi += pij;
            if (i == j) {
                rho += pij;
            } else {
                double thetaII = 0.0, thetaJJ = 0.0;
                for (Size t = 0; t < T; ++t) {
                    const double d = y[i][t] * y[j][t] - S[i][j];
                    thetaII += (y[i][t] * y[i][t] - S[i][i]) * d;
                    thetaJJ += (y[j][t] * y[j][t] - S[j][j]) * d;
                }
                thetaII /= Td;
                thetaJJ /= Td;
                rho += (out.target / 2.0) * (std::sqrt(S[j][j] / S[i][i]) * thetaII +
                                             std::sqrt(S[i][i] / S[j][j]) * thetaJJ);
                const double f = out.target * std::sqrt(S[i][i] * S[j][j]);
                gamma += (f - S[i][j]) * (f - S[i][j]);
            }
        }
    }
    if (!(gamma > 0.0)) return out;
    const double kappa = (pi - rho) / gamma;
    out.intensity = std::max(0.0, std::min(1.0, kappa / Td));
    return out;
}

double smallestEigenvalue(const Matrix& c) {
    const QuantLib::SymmetricSchurDecomposition schur(c);
    const QuantLib::Array& ev = schur.eigenvalues();
    double smallest = ev[0];
    for (Size l = 1; l < c.rows(); ++l)
        smallest = std::min(smallest, ev[l]);
    return smallest;
}

FlooredMatrix floorEigenvalues(const Matrix& c, double floor) {
    const Size n = c.rows();
    FlooredMatrix out;
    const QuantLib::SymmetricSchurDecomposition schur(c);
    const QuantLib::Array& ev = schur.eigenvalues();
    out.smallestBefore = ev[0];
    for (Size l = 1; l < n; ++l)
        out.smallestBefore = std::min(out.smallestBefore, ev[l]);
    if (out.smallestBefore >= -kPsdTolerance) {
        out.matrix = c;
        out.smallestAfter = out.smallestBefore;
        return out;
    }
    out.applied = true;
    const Matrix& v = schur.eigenvectors();
    // Upper triangle computed, lower mirrored: the sum is the same algebra
    // either way, but not the same rounding, and the spec must be symmetric
    // to the bit.
    Matrix r(n, n, 0.0);
    for (Size i = 0; i < n; ++i) {
        for (Size j = i; j < n; ++j) {
            double sum = 0.0;
            for (Size l = 0; l < n; ++l)
                sum += v[i][l] * std::max(ev[l], floor) * v[j][l];
            r[i][j] = sum;
            r[j][i] = sum;
        }
    }
    out.matrix = Matrix(n, n, 1.0);
    for (Size i = 0; i < n; ++i) {
        for (Size j = i + 1; j < n; ++j) {
            out.matrix[i][j] = r[i][j] / std::sqrt(r[i][i] * r[j][j]);
            out.matrix[j][i] = out.matrix[i][j];
        }
    }
    out.smallestAfter = smallestEigenvalue(out.matrix);
    return out;
}

CorrelationCalibrationResult CorrelationCalibrationEvaluator::evaluate(
    const CorrelationCalibrationInputs& in,
    const PricingRegistry& /*reg*/,
    const PricingContext& /*ctx*/) const {
    if (in.asOf == Date()) {
        QUANTRA_INVALID_ARGUMENT("as_of_date is required");
    }
    if (in.correlationId.empty()) {
        QUANTRA_INVALID_ARGUMENT("correlation_id is required");
    }
    validateSeries(in.series);
    validateEstimator(in.estimator);
    const std::vector<std::size_t> order = outputOrder(in.series, in.labels);
    const CorrelationEstimatorData& est = in.estimator;
    const std::size_t n = in.series.size();

    // --- The window.
    std::optional<Date> lo = est.startDate, hi = est.endDate;
    if (est.windowDays) {
        lo = in.asOf - static_cast<QuantLib::Integer>(*est.windowDays);
        hi = in.asOf;
    }

    // --- Inner join on dates inside the window.
    std::map<Date, std::size_t> count;
    std::vector<std::map<Date, double>> byDate(n);
    std::vector<std::map<Date, std::string>> isoByDate(n);
    for (std::size_t i = 0; i < n; ++i) {
        for (const auto& c : in.series[i].closes) {
            if (lo && c.date < *lo) continue;
            if (hi && c.date > *hi) continue;
            byDate[i][c.date] = c.value;
            isoByDate[i][c.date] = c.dateIso;
            ++count[c.date];
        }
    }
    std::vector<Date> common;
    for (const auto& kv : count)
        if (kv.second == n) common.push_back(kv.first);
    const std::string windowText = "[" + (lo ? DateToIso(*lo) : std::string("open")) + ", " +
                                   (hi ? DateToIso(*hi) : std::string("open")) + "]";
    if (common.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            "estimator window " + windowText + " contains no date common to every series");
    }
    if (common.size() < est.minOverlap) {
        QUANTRA_INVALID_ARGUMENT(
            "overlap of " + std::to_string(common.size()) + " common dates inside the window " +
            windowText + " is below estimator.min_overlap (" + std::to_string(est.minOverlap) +
            ")");
    }

    // --- Frequency sampling: every fifth common observation from the last.
    const unsigned step = est.frequency == ReturnSampling::Weekly ? kWeeklyStep : 1u;
    std::vector<Date> used;
    for (std::size_t k = common.size(); k >= 1;) {
        used.push_back(common[k - 1]);
        if (k - 1 < step) break;
        k -= step;
    }
    std::reverse(used.begin(), used.end());
    if (used.size() < 3) {
        QUANTRA_INVALID_ARGUMENT(
            "fewer than 3 observations after the " + std::string(step == 1 ? "Daily" : "Weekly") +
            " sampling of " + std::to_string(common.size()) + " common dates");
    }
    const Size T = used.size() - 1;

    // --- Returns and demeaned returns, series order.
    std::vector<std::vector<double>> y(n, std::vector<double>(T, 0.0));
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<double> x(T);
        for (Size t = 0; t < T; ++t) {
            const double p0 = byDate[i][used[t]];
            const double p1 = byDate[i][used[t + 1]];
            x[t] = est.returns == ReturnKind::Log ? std::log(p1 / p0) : p1 / p0 - 1.0;
        }
        double sum = 0.0;
        for (Size t = 0; t < T; ++t)
            sum += x[t];
        const double mean = sum / static_cast<double>(T);
        for (Size t = 0; t < T; ++t)
            y[i][t] = x[t] - mean;
    }

    // --- Moments, shrinkage, floor.
    const SampleMoments m = sampleMoments(y);
    const LedoitWolfEstimate lw = ledoitWolfConstantCorrelation(y, m);
    double delta = 0.0;
    if (est.shrinkage == ShrinkageKind::LedoitWolf) delta = lw.intensity;
    if (est.shrinkage == ShrinkageKind::Fixed) delta = *est.shrinkageIntensity;
    Matrix shrunk(n, n, 1.0);
    for (Size i = 0; i < n; ++i)
        for (Size j = 0; j < n; ++j)
            if (i != j) shrunk[i][j] = delta * lw.target + (1.0 - delta) * m.correlation[i][j];
    const FlooredMatrix floored = floorEigenvalues(shrunk, est.eigenvalueFloor);

    // --- Result in the output order.
    CorrelationCalibrationResult r;
    r.correlationId = in.correlationId;
    for (std::size_t a = 0; a < n; ++a)
        r.labels.push_back(in.series[order[a]].underlyingId);
    for (std::size_t a = 0; a < n; ++a) {
        for (std::size_t b = 0; b < n; ++b) {
            r.correlation.push_back(floored.matrix[order[a]][order[b]]);
            r.sampleCorrelation.push_back(m.correlation[order[a]][order[b]]);
        }
    }
    r.shrinkageTarget = lw.target;
    r.shrinkageIntensityUsed = delta;
    r.eigenvalueFloorApplied = floored.applied;
    r.smallestEigenvalueBefore = floored.smallestBefore;
    r.smallestEigenvalueAfter = floored.smallestAfter;
    r.observationsUsed = static_cast<unsigned>(used.size());
    r.returnsUsed = static_cast<unsigned>(T);
    r.firstDateIso = isoByDate[0][used.front()];
    r.lastDateIso = isoByDate[0][used.back()];
    const double annualisation = kTradingDaysPerYear / static_cast<double>(step) *
                                 static_cast<double>(T) / static_cast<double>(T - 1);
    for (std::size_t a = 0; a < n; ++a) {
        const std::size_t i = order[a];
        CorrelationSeriesReportData s;
        s.underlyingId = in.series[i].underlyingId;
        s.observations = static_cast<unsigned>(in.series[i].closes.size());
        s.dropped = s.observations - r.observationsUsed;
        s.annualisedVolatility = std::sqrt(m.covariance[i][i] * annualisation);
        r.series.push_back(s);
    }
    r.estimatorEcho = est;
    r.estimatorEcho.windowDays = std::nullopt;
    r.estimatorEcho.startDate = lo ? lo : std::optional<Date>(used.front());
    r.estimatorEcho.endDate = hi ? hi : std::optional<Date>(used.back());
    if (est.shrinkage != ShrinkageKind::Fixed) r.estimatorEcho.shrinkageIntensity = std::nullopt;
    return r;
}

} // namespace quantra
