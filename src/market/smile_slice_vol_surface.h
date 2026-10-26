#ifndef QUANTRA_SMILE_SLICE_VOL_SURFACE_H
#define QUANTRA_SMILE_SLICE_VOL_SURFACE_H

/**
 * Black volatility surfaces built from one smile per expiry.
 *
 * A `SmileSliceVolSurface` is a QuantLib BlackVolTermStructure (through
 * BlackVarianceTermStructure, so blackVol is sqrt(variance / t)) whose total
 * variance at (t, K) is interpolated LINEARLY IN TIME at fixed strike between
 * the slices bracketing t:
 *
 *     w(t, K) = w_i(K) + (w_{i+1}(K) - w_i(K)) (t - T_i) / (T_{i+1} - T_i),
 *
 * with w(t, K) = w_1(K) t / T_1 before the first slice and w_n(K) t / T_n
 * after the last (a flat implied vol on both ends; the same rule QuantLib's
 * BlackVarianceSurface applies through its zero-variance column at t = 0 and
 * beyond its last expiry). T_i is the day counter's year fraction from the
 * reference date to the slice's expiry. Each slice is one of
 *
 *   * `CubicVarianceSlice`: the grid row of a `BlackVolSpec` surface read
 *     with `surface_interpolator: CubicStrikeLinearTime` — a natural cubic
 *     spline (QuantLib CubicNaturalSpline: C2, zero second derivative at the
 *     end nodes) of total variance sigma_j^2 T in ln K_j over the grid
 *     strikes, constant beyond the first and last strike (the edge node's
 *     variance; strike extrapolation as the bilinear surface's
 *     ConstantExtrapolation). The spline reproduces every node exactly and
 *     keeps the smile's curvature between nodes, which a bilinear reading
 *     loses (the d2w/dk2 term of Dupire's local vol is then zero inside every
 *     cell). Natural rather than a monotone (Kruger) scheme: total variance
 *     along a smile is not monotone, a monotone scheme is only C1 (the local
 *     vol would jump at every node) and zeroes the slope at the node nearest
 *     the smile's minimum, exactly where Dupire reads the curvature.
 *   * `SviSmileSlice`: the raw SVI total variance
 *     w(k) = a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2)), k = ln(K / F),
 *     the parameterisation of QuantLib's SviSmileSection and of the
 *     `/calibrate-vol-surface` SviPerExpiry fit; global in strike, no
 *     extrapolation (the strike is floored at 1e-6 as QuantLib does).
 *
 * The surface is strike-dependent, so a structured note simulates it as
 * Dupire local vol (QuantLib LocalVolSurface over this object; see
 * src/mc/basket_model.h). It is cheap to read: two 1-D evaluations per call.
 *
 * `ShiftedBlackVolSurface` adds a constant to every Black vol of another
 * surface (w' = (sigma + shift)^2 t): the vega bump of an SVI payload, whose
 * uniformly shifted vol is not an SVI slice and cannot be rebuilt from
 * parameters.
 *
 * QuantLib only: no FlatBuffers here.
 */

#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>

#include <memory>
#include <vector>

namespace quantra {

/// One expiry's total variance as a function of the absolute strike.
class SmileSlice {
public:
    virtual ~SmileSlice() = default;
    virtual double totalVariance(double strike) const = 0;
};

/// Natural cubic spline of total variance in log-strike over grid nodes,
/// constant outside the node range.
class CubicVarianceSlice : public SmileSlice {
public:
    /// `strikes` strictly increasing (at least 2), `totalVariances` the
    /// nodes' sigma^2 T.
    CubicVarianceSlice(const std::vector<double>& strikes, std::vector<double> totalVariances);
    double totalVariance(double strike) const override;

private:
    std::vector<double> logStrikes_;
    std::vector<double> variances_;
    std::unique_ptr<QuantLib::CubicInterpolation> spline_;
};

struct SviSliceParams {
    double forward = 0.0;
    double a = 0.0, b = 0.0, sigma = 0.0, rho = 0.0, m = 0.0;
};

/// Raw SVI total variance a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2)),
/// in exactly that evaluation order (the Python reference repeats it).
double sviRawTotalVariance(double a, double b, double sigma, double rho, double m, double k);

/// Raw SVI slice about its forward.
class SviSmileSlice : public SmileSlice {
public:
    explicit SviSmileSlice(const SviSliceParams& p) : p_(p) {}
    double totalVariance(double strike) const override;
    const SviSliceParams& params() const { return p_; }

private:
    SviSliceParams p_;
};

class SmileSliceVolSurface : public QuantLib::BlackVarianceTermStructure {
public:
    /// `expiries` strictly increasing and after `referenceDate`, one slice
    /// each (at least one). `minStrike` / `maxStrike` are what direct readers
    /// without extrapolation are limited to (the grid's edges for cubic
    /// slices, 0 / QL_MAX_REAL for SVI).
    SmileSliceVolSurface(
        const QuantLib::Date& referenceDate,
        const QuantLib::Calendar& calendar,
        QuantLib::BusinessDayConvention bdc,
        const QuantLib::DayCounter& dayCounter,
        std::vector<QuantLib::Date> expiries,
        std::vector<std::shared_ptr<const SmileSlice>> slices,
        QuantLib::Real minStrike,
        QuantLib::Real maxStrike);

    QuantLib::Date maxDate() const override { return expiries_.back(); }
    QuantLib::Real minStrike() const override { return minStrike_; }
    QuantLib::Real maxStrike() const override { return maxStrike_; }
    const std::vector<QuantLib::Date>& expiries() const { return expiries_; }
    const std::vector<QuantLib::Time>& times() const { return times_; }
    const std::vector<std::shared_ptr<const SmileSlice>>& slices() const { return slices_; }

protected:
    QuantLib::Real blackVarianceImpl(QuantLib::Time t, QuantLib::Real strike) const override;

private:
    std::vector<QuantLib::Date> expiries_;
    std::vector<QuantLib::Time> times_;
    std::vector<std::shared_ptr<const SmileSlice>> slices_;
    QuantLib::Real minStrike_, maxStrike_;
};

/// Every Black vol of `inner` plus `shift` (vol units). Throws
/// QuantLib::Error when a shifted vol is not positive where it is read.
class ShiftedBlackVolSurface : public QuantLib::BlackVarianceTermStructure {
public:
    ShiftedBlackVolSurface(
        std::shared_ptr<const QuantLib::BlackVolTermStructure> inner, double shift);

    const QuantLib::Date& referenceDate() const override { return inner_->referenceDate(); }
    QuantLib::Date maxDate() const override { return inner_->maxDate(); }
    QuantLib::Real minStrike() const override { return inner_->minStrike(); }
    QuantLib::Real maxStrike() const override { return inner_->maxStrike(); }

protected:
    QuantLib::Real blackVarianceImpl(QuantLib::Time t, QuantLib::Real strike) const override;

private:
    std::shared_ptr<const QuantLib::BlackVolTermStructure> inner_;
    double shift_;
};

/**
 * The `CubicStrikeLinearTime` reading of a strike x expiry Black vol grid:
 * `blackVols` is (strikes x expiries) as QuantLib's BlackVarianceSurface
 * takes it. Throws QuantLib::Error on fewer than 2 strikes, unsorted axes
 * or a non-positive vol.
 */
std::shared_ptr<SmileSliceVolSurface> makeCubicStrikeLinearTimeSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<double>& strikes,
    const std::vector<std::vector<double>>& blackVolsByExpiry);

/// An SVI surface: one `SviSmileSlice` per expiry (same order). Throws
/// QuantLib::Error on an inadmissible slice.
std::shared_ptr<SmileSliceVolSurface> makeSviSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<SviSliceParams>& slices);

} // namespace quantra

#endif // QUANTRA_SMILE_SLICE_VOL_SURFACE_H
