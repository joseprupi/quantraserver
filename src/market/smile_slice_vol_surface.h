#ifndef QUANTRA_SMILE_SLICE_VOL_SURFACE_H
#define QUANTRA_SMILE_SLICE_VOL_SURFACE_H

/**
 * Black volatility surfaces built from one smile per expiry.
 *
 * A `SmileSliceVolSurface` is a QuantLib BlackVolTermStructure (through
 * BlackVarianceTermStructure, so blackVol is sqrt(variance / t)) whose total
 * variance at (t, K) is interpolated LINEARLY IN TIME between the slices
 * bracketing t, in one of two readings:
 *
 *   * at fixed strike (no surface forward; the reading before 0.16.10 and
 *     still the default of a request that does not ask for the other):
 *
 *         w(t, K) = w_i(K) + (w_{i+1}(K) - w_i(K)) (t - T_i) / (T_{i+1} - T_i);
 *
 *   * at fixed forward moneyness k = ln(K / F(t)) (a `SurfaceForward` F
 *     given; since 0.16.10): each slice is read at the strike with the same
 *     moneyness against its own forward F_i = F(T_i),
 *
 *         K_i = K (F_i / F(t)),
 *         w(t, K) = w_i(K_i) + (w_{i+1}(K_{i+1}) - w_i(K_i)) (t - T_i) / (T_{i+1} - T_i).
 *
 *     QuantLib's LocalVolSurface differentiates the variance in time along
 *     the forward (its stencil reads K F(t + dt) / F(t)); along a forward
 *     line with the surface's own drift k is constant, so the time
 *     derivative is (w_{i+1}(k) - w_i(k)) / (T_{i+1} - T_i) whatever the
 *     strike: slices ordered at fixed moneyness give a non-negative Dupire
 *     numerator everywhere between them. At fixed strike the same slices
 *     can show a decreasing variance along that line.
 *
 * Before the first slice w(t, K) = w_1(K_1) t / T_1 and after the last
 * w_n(K_n) t / T_n (a flat implied vol on both ends; the same rule QuantLib's
 * BlackVarianceSurface applies through its zero-variance column at t = 0 and
 * beyond its last expiry; K_i = K at fixed strike). T_i is the day counter's
 * year fraction from the reference date to the slice's expiry. Each slice is
 * one of
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
 *     extrapolation (the strike is floored at 1e-6 as QuantLib does). The
 *     quoted strike range a payload may carry (`strike_min` / `strike_max`)
 *     is not part of the surface: it travels on the BlackVolEntry to the
 *     structured note's local vol (src/mc/basket_model.h), which holds its
 *     edge value beyond it; every Black vol read ignores it.
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

#include <ql/handle.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>

#include <memory>
#include <optional>
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

/// The forward a surface measures its moneyness against, F(t) at time t
/// (year fraction from the surface's reference date).
class SurfaceForward {
public:
    virtual ~SurfaceForward() = default;
    virtual double forward(QuantLib::Time t) const = 0;
};

/**
 * Forwards given at nodes (the convention of the eSSVI payload's reference
 * forward): ln F linear in t through (0, ln spot) when a spot is given and
 * (T_i, ln F_i), the last segment's slope continued beyond the last node (a
 * flat forward rate); without a spot the first segment's slope is continued
 * back before the first node (one node and no spot: F constant). F(T_i) is
 * F_i exactly. Throws QuantLib::Error on a non-positive or non-finite value
 * or times not strictly increasing and positive.
 */
class NodeSurfaceForward : public SurfaceForward {
public:
    NodeSurfaceForward(
        const std::vector<QuantLib::Time>& times,
        const std::vector<double>& forwards,
        std::optional<double> spot);
    double forward(QuantLib::Time t) const override;

private:
    std::vector<double> times_, logForwards_, forwards_;
};

/// spot D_q(t) / D_r(t) on a dividend and a risk-free curve (the curves are
/// read at every call; the spot is a snapshot, as the eSSVI surface's).
class CurveSurfaceForward : public SurfaceForward {
public:
    CurveSurfaceForward(
        double spot,
        QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
        QuantLib::Handle<QuantLib::YieldTermStructure> riskFree);
    double forward(QuantLib::Time t) const override;

private:
    double spot_;
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend_, riskFree_;
};

class SmileSliceVolSurface : public QuantLib::BlackVarianceTermStructure {
public:
    /// `expiries` strictly increasing and after `referenceDate`, one slice
    /// each (at least one). `minStrike` / `maxStrike` are what direct readers
    /// without extrapolation are limited to (the grid's edges for cubic
    /// slices, 0 / QL_MAX_REAL for SVI). `forward` null: the fixed-strike
    /// reading; given: the fixed-forward-moneyness reading against it.
    SmileSliceVolSurface(
        const QuantLib::Date& referenceDate,
        const QuantLib::Calendar& calendar,
        QuantLib::BusinessDayConvention bdc,
        const QuantLib::DayCounter& dayCounter,
        std::vector<QuantLib::Date> expiries,
        std::vector<std::shared_ptr<const SmileSlice>> slices,
        QuantLib::Real minStrike,
        QuantLib::Real maxStrike,
        std::shared_ptr<const SurfaceForward> forward = nullptr);

    QuantLib::Date maxDate() const override { return expiries_.back(); }
    QuantLib::Real minStrike() const override { return minStrike_; }
    QuantLib::Real maxStrike() const override { return maxStrike_; }
    const std::vector<QuantLib::Date>& expiries() const { return expiries_; }
    const std::vector<QuantLib::Time>& times() const { return times_; }
    const std::vector<std::shared_ptr<const SmileSlice>>& slices() const { return slices_; }
    /// The surface forward of the fixed-forward-moneyness reading (null at
    /// fixed strike).
    const std::shared_ptr<const SurfaceForward>& surfaceForward() const { return forward_; }

protected:
    QuantLib::Real blackVarianceImpl(QuantLib::Time t, QuantLib::Real strike) const override;

private:
    std::vector<QuantLib::Date> expiries_;
    std::vector<QuantLib::Time> times_;
    std::vector<std::shared_ptr<const SmileSlice>> slices_;
    QuantLib::Real minStrike_, maxStrike_;
    std::shared_ptr<const SurfaceForward> forward_;
    std::vector<double> sliceForwards_; // F(T_i), forward-moneyness reading only
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
 * The `CubicStrikeLinearTime` reading of a strike x expiry Black vol grid
 * (`forward` null) or the `CubicStrikeLinearTimeForwardMoneyness` one
 * (`forward` given): `blackVolsByExpiry` is one row per expiry. Throws
 * QuantLib::Error on fewer than 2 strikes, unsorted axes or a non-positive
 * vol.
 */
std::shared_ptr<SmileSliceVolSurface> makeCubicStrikeLinearTimeSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<double>& strikes,
    const std::vector<std::vector<double>>& blackVolsByExpiry,
    std::shared_ptr<const SurfaceForward> forward = nullptr);

/// Time reading of an SVI surface (enums::SviTimeInterpolation).
enum class SviTimeReading { FixedStrike, FixedForwardMoneyness };

/// An SVI surface: one `SviSmileSlice` per expiry (same order). With
/// FixedForwardMoneyness the surface forward is the NodeSurfaceForward
/// through the slices' forwards (and `spot` at t = 0 when given). Throws
/// QuantLib::Error on an inadmissible slice.
std::shared_ptr<SmileSliceVolSurface> makeSviSurface(
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& calendar,
    QuantLib::BusinessDayConvention bdc,
    const QuantLib::DayCounter& dayCounter,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<SviSliceParams>& slices,
    SviTimeReading reading = SviTimeReading::FixedStrike,
    std::optional<double> spot = std::nullopt);

} // namespace quantra

#endif // QUANTRA_SMILE_SLICE_VOL_SURFACE_H
