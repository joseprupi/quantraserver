#ifndef QUANTRA_MC_BASKET_MODEL_H
#define QUANTRA_MC_BASKET_MODEL_H

/**
 * Basket model for the shared Monte Carlo core: N correlated lognormal
 * (Black-Scholes / local-vol) assets as one QuantLib StochasticProcessArray.
 *
 * This module knows nothing about products. A caller (an evaluator) resolves
 * its underlyings from the pricing registries, builds one BasketAsset per
 * underlying and gets back the process array that `path_engine` simulates.
 *
 * Per asset the process is
 *
 *     GeneralizedBlackScholesProcess(spot, dividend, riskFree, vol)
 *
 * (ql/processes/blackscholesprocess.hpp). The drift of the asset in the
 * simulation measure is riskFree - dividend, so:
 *
 *   * `riskFree` MUST be the discounting curve of the payoff currency (the
 *     note curve). Every asset of a basket is simulated in that one measure.
 *   * a plain (payoff-currency) asset passes its own dividend curve;
 *   * a quanto asset (simulated in its own currency, paid in the note
 *     currency) passes the curve from `buildQuantoDividendCurve`
 *     (src/market/quanto_builder.h) as `dividend` and the SAME note curve as
 *     `riskFree`, so that the drift is r_local - q - rho * sigma_S * sigma_FX.
 *
 * `exactStepCapable` records whether QuantLib evolves the asset with the exact
 * lognormal step between two grid points (the vol is a BlackConstantVol or a
 * BlackVarianceCurve: strike-independent, QuantLib integrates the variance
 * exactly) or with a log-Euler step on the Dupire local vol derived from a
 * strike x expiry surface (BlackVarianceSurface or any other
 * BlackVolTermStructure). It is derived from the dynamic type of the vol
 * object, exactly the test GeneralizedBlackScholesProcess::localVolatility()
 * performs; it is not an input. A basket that is not exact-step capable
 * needs a fine time grid (`stepsPerYear`, see time_grid.h): with one step per
 * observation date the local-vol evolution is wrong (frozen at the local vol
 * of the starting point).
 *
 * The processes keep the handles (no copies): a relinked or bumped spot, curve
 * or vol flows through to the next simulation (bump-and-reprice Greeks with
 * the same seed).
 *
 * Exact-step table. When every asset is exact-step capable, QuantLib's step
 * (GeneralizedBlackScholesProcess::evolve, ql/processes/blackscholesprocess.cpp)
 * is x * exp((r - q) dt - var / 2 + sqrt(var) dz) with r, q and var read from
 * the curves and the vol for (t0, dt) only: deterministic per (asset, step),
 * yet QuantLib re-reads them for every path (90 % of the simulation time on a
 * quanto basket, measured with the benchmark). `LognormalBasket::stepTable` hoists
 * those reads into a table once per run; the path engine then applies the
 * same formula in the same evaluation order, so the paths are bit-identical
 * to MultiPathGenerator's (verified level by level in
 * tests/parity/mc_core_test.cpp). A basket with a local-vol asset has no
 * table: its log-Euler step depends on the simulated level.
 *
 * A smile surface read at fixed forward moneyness (since 0.16.10) is handed
 * to LocalVolSurface as localVolReadingSurface makes it
 * (src/market/smile_slice_vol_surface.h): its forward reshaped between
 * expiries like the process forward, equal to the surface's at every
 * expiry, so the stencil's time derivative stays at fixed moneyness.
 *
 * Local vol of a strike x expiry surface. GeneralizedBlackScholesProcess
 * derives its Dupire local vol lazily (LocalVolSurface(blackVol, riskFree,
 * dividend, spot) on first use, one per process instance) and evaluates it
 * at the simulated level, wherever the path went. Outside the quoted
 * strikes that is undefined territory: LocalVolSurface differentiates the
 * Black variance with a finite-difference stencil (dy = 1e-4 |ln(K/F)| in
 * log-strike, ~1e-6 near the money), and at the edge of the strike grid the
 * stencil straddles the surface's extrapolation kink. With the engine's
 * constant strike extrapolation (the edge vol held) that kink is concave on
 * a skewed wing and the Dupire denominator turns negative ("negative local
 * vol^2 at strike ..." from QuantLib); with QuantLib's default linear
 * continuation of the variance it is the variance itself that turns
 * negative far away. Either way the surface knows nothing beyond its last
 * strike. `EdgeClampedLocalVol` therefore holds the LOCAL vol at the edge:
 * the level is clamped to [minStrike e^m, maxStrike e^-m] (m = kEdgeMargin
 * = 1e-3 in log-strike, wider than the stencil) before LocalVolSurface is
 * asked, so a path beyond the quoted strikes evolves with the edge
 * strike's local vol and the stencil never leaves the grid. Inside that
 * interior the value is LocalVolSurface's own, bit for bit. An SVI surface
 * is global in strike (minStrike 0, maxStrike QL_MAX_REAL: no clamp) unless
 * its payload carries the quoted strike range (`strike_min` /
 * `strike_max`), passed as `BasketAsset::localVolStrikeRange`: a raw SVI
 * wing beyond the quotes can carry butterfly arbitrage, so the local vol
 * is then held at the quoted edge exactly as for a grid.
 *
 * Local vol beyond the last expiry. A strike x expiry surface extends its
 * IMPLIED vol flat in time beyond its last expiry (w(t, K) = w(T, K) t / T).
 * With a skew that is not arbitrage-free at long horizons: the Dupire
 * denominator carries -t^2 v_k^2 / 16 (v = w / t the implied variance, k
 * the log-strike), which eventually dominates and turns the local variance
 * negative ("negative local vol^2 at strike ... and time ..."). With
 * `BasketAsset::localVolTime == FlatLocalVol` the local vol is instead held
 * time-homogeneous beyond the last expiry: it is read at
 * min(t, T_last - kTimeMargin), T_last the Black surface's maxTime(), the
 * margin (1e-3 years) wider than LocalVolSurface's time stencil (t +/- dt,
 * dt = min(1e-4, t / 2)) so the stencil stays inside the quoted expiries.
 * Before that time the value is LocalVolSurface's own, bit for bit. The
 * Black vol itself (vanilla pricing, diagnostics) is unchanged.
 *
 * Per-expiry strike bands. `BasketAsset::localVolStrikeBands` (from a
 * BlackVolSpec's local_vol_strike_min / _max or an SviVolSpec /
 * SsviVolSpec with local_vol_strike_range PerExpiry) replace the single
 * strike range. The band of a time x is the band of the first expiry at or
 * after x (the last band beyond the last expiry): between two expiries the
 * later one's, the range over which both slices are checked. A read at t
 * evaluates the Black variance at every time of LocalVolSurface's stencil
 * (t +/- dt, dt <= 1e-4), so the level is clamped to the INTERSECTION of
 * the bands of the times within kBandTimeWindow (2e-4 years) of t
 * (strikeBand): inside an interval its one band, within the window of an
 * expiry the bands of both intervals around it. A step landing exactly on
 * an expiry therefore never reads the next slice beyond the next band
 * (before 0.16.9 it used the earlier band alone, and where that reached
 * further than the next one the stencil read the next slice on its
 * unchecked wing). Disjoint bands fall back to the band of t itself. The
 * time used is the time read (after the FlatLocalVol clamp).
 *
 * Local vol floor. With `BasketAsset::localVolFloor` the local vol is
 * computed in EdgeClampedLocalVol::dupire with QuantLib's
 * LocalVolSurface::localVolImpl formula, step by step (the same reads,
 * the same arithmetic in the same order: bit-identical where the floor
 * does not bind), the simulation reads max(Dupire, floor), and a read
 * QuantLib would refuse returns the floor; reads, floored reads and
 * refusals are counted for the response.
 *
 * eSSVI surfaces (src/market/ssvi_vol_surface.h). For an SsviBlackVol the
 * inner local vol is SsviLocalVol, the closed-form Dupire local variance of
 * the slices (w_t + w_k dDrift) / g(k, y) with the sticky-strike correction
 * for a process forward that differs from the surface's reference forward
 * (a bumped spot, a quanto drift, another dividend curve), instead of
 * QuantLib's finite-difference LocalVolSurface; no stencil, so no strike or
 * time margin is needed, but the same strike range / bands, FlatLocalVol
 * clamp (at the last slice's time) and floor apply to it unchanged. In floor
 * mode dupire() returns its value (NaN where it refuses: g <= 0 or a negative
 * numerator).
 *
 * Sharing. A request builds several process arrays from the same market
 * (the price, the correlation limits of `checks`, the
 * bumped runs whose inputs did not move), each deriving its own local vol
 * copy. `LocalVolCache` hands out ONE edge-clamped LocalVolSurface per
 * distinct (vol, spot, dividend, riskFree) quadruple and
 * `BasketAsset::localVol` carries it into the process (QuantLib's
 * external-local-vol constructor), so every process built from the same
 * market shares the object. The Euler step is QuantLib's own either way:
 * for levels inside the clamp the paths are bit-identical to the lazily
 * derived ones (tests/parity/mc_core_test.cpp).
 *
 * QuantLib only: no FlatBuffers, no gRPC, no registries in this module.
 */

#include <ql/handle.hpp>
#include <ql/math/matrix.hpp>
#include <ql/processes/stochasticprocessarray.hpp>
#include <ql/quote.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/volatility/equityfx/localvoltermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/timegrid.hpp>
#include <ql/utilities/null.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace quantra { namespace mc {

/// How the local vol of a strike-dependent vol is extended in time beyond
/// the Black surface's last expiry (see the file comment).
enum class LocalVolTimeExtrapolation {
    /// The local vol derived from the surface's own flat-implied-vol
    /// extension (no time clamp).
    FlatImpliedVol,
    /// The local vol at T_last - kTimeMargin for every later time.
    FlatLocalVol
};

/// A strike range the local vol is held to up to an expiry time.
struct LocalVolStrikeBand {
    QuantLib::Time time = 0.0;
    QuantLib::Real minStrike = 0.0;
    QuantLib::Real maxStrike = 0.0;
};

/// One simulated asset. All handles must be linked.
struct BasketAsset {
    /// Caller's label (underlying id); must be unique within a basket. Used
    /// in error messages only.
    std::string id;
    QuantLib::Handle<QuantLib::Quote> spot;
    /// Dividend-yield curve, or the quanto-adjusted curve for a quanto asset.
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend;
    /// The discounting / payoff-currency curve (the same handle for every
    /// asset of a basket).
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree;
    QuantLib::Handle<QuantLib::BlackVolTermStructure> vol;
    /// True iff `vol` is strike-independent (BlackConstantVol or
    /// BlackVarianceCurve), i.e. QuantLib's step between two grid times is the
    /// exact lognormal step. Set it with IsExactStepCapable(vol) /
    /// MakeBasketAsset; BasketModel::build rejects a value that contradicts
    /// the vol's type.
    bool exactStepCapable = false;
    /// Optional: the Dupire local vol of a strike-dependent `vol`, shared
    /// between the processes of one request (LocalVolCache). Empty = the
    /// process derives its own on first use. Must be empty for an
    /// exact-step asset (QUANTRA_ERROR otherwise: caller wiring bug).
    QuantLib::Handle<QuantLib::LocalVolTermStructure> localVol;
    /// Time extension of the local vol of a strike-dependent `vol`
    /// (ignored for an exact-step asset). LocalVolCache applies it.
    LocalVolTimeExtrapolation localVolTime = LocalVolTimeExtrapolation::FlatImpliedVol;
    /// Optional: the strike range the local vol of a strike-dependent `vol`
    /// is clamped to (the quoted strikes of an SVI payload). Empty = the
    /// vol's minStrike() / maxStrike(). LocalVolCache applies it.
    std::optional<std::pair<QuantLib::Real, QuantLib::Real>> localVolStrikeRange;
    /// Optional: per-expiry strike ranges (expiry time, min strike, max
    /// strike; times strictly increasing, one per expiry of `vol`). When
    /// given they replace `localVolStrikeRange`: at time t the level is
    /// clamped to the band of the first expiry at or after t, intersected
    /// with the neighbouring band within the stencil window of an expiry
    /// (EdgeClampedLocalVol::strikeBand). LocalVolCache applies it.
    std::vector<LocalVolStrikeBand> localVolStrikeBands;
    /// Optional local vol floor (> 0): the local vol simulated is
    /// max(Dupire, floor), and a read where QuantLib's LocalVolSurface would
    /// refuse (decreasing variance, negative local variance) is the floor
    /// instead of an error (EdgeClampedLocalVol, counted). Empty = QuantLib's
    /// LocalVolSurface as is (refusals propagate). LocalVolCache applies it.
    std::optional<QuantLib::Real> localVolFloor;
};

/**
 * Whether QuantLib evolves a process with this vol exactly between grid
 * points: true for BlackConstantVol and BlackVarianceCurve (the two types
 * GeneralizedBlackScholesProcess treats as strike-independent), false for a
 * BlackVarianceSurface or any other BlackVolTermStructure (Dupire local vol,
 * log-Euler steps). An empty handle is a server error (QUANTRA_ERROR).
 */
bool IsExactStepCapable(const QuantLib::Handle<QuantLib::BlackVolTermStructure>& vol);

/// Build a BasketAsset with `exactStepCapable` derived from `vol`.
BasketAsset MakeBasketAsset(
    const std::string& id,
    const QuantLib::Handle<QuantLib::Quote>& spot,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree,
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& vol);

/**
 * QuantLib's LocalVolSurface evaluated at the level clamped to the interior
 * of the Black surface's strike grid and, optionally, at the time clamped
 * below the surface's last expiry (see the file comment). Every term
 * structure property is the inner surface's.
 */
class EdgeClampedLocalVol : public QuantLib::LocalVolTermStructure {
public:
    /// Clamp margin in log-strike on each side of the quoted strike range.
    static constexpr double kEdgeMargin = 1.0e-3;
    /// Clamp margin in years below the last expiry (FlatLocalVol): wider
    /// than LocalVolSurface's time stencil (dt <= 1e-4).
    static constexpr double kTimeMargin = 1.0e-3;
    /// Half-width of the time window whose strike bands are intersected
    /// (per-expiry bands): twice LocalVolSurface's time stencil.
    static constexpr double kBandTimeWindow = 2.0e-4;

    /// `maxTime`: the Black surface's maxTime() to hold the local vol flat
    /// beyond (FlatLocalVol), or QuantLib::Null<Time>() for no time clamp. A
    /// maxTime not above 2 kTimeMargin leaves no interior: no time clamp.
    EdgeClampedLocalVol(
        std::shared_ptr<QuantLib::LocalVolTermStructure> inner,
        QuantLib::Real minStrike,
        QuantLib::Real maxStrike,
        QuantLib::Time maxTime = QuantLib::Null<QuantLib::Time>());

    /// Per-expiry strike bands (see BasketAsset::localVolStrikeBands), each
    /// with the same margin; at least one band, times strictly increasing.
    EdgeClampedLocalVol(
        std::shared_ptr<QuantLib::LocalVolTermStructure> inner,
        const std::vector<LocalVolStrikeBand>& bands,
        QuantLib::Time maxTime = QuantLib::Null<QuantLib::Time>());

    /// The level the inner local vol is read at for a simulated level `s`
    /// (at read time `t` when the clamp has per-expiry bands).
    QuantLib::Real clampedLevel(QuantLib::Real s, QuantLib::Time t = 0.0) const;
    /// The strike interval a read at time `t` (the time read, after
    /// clampedTime) is clamped to, margins applied: the intersection of the
    /// per-expiry bands within kBandTimeWindow of t (see the file comment),
    /// or the single range.
    std::pair<QuantLib::Real, QuantLib::Real> strikeBand(QuantLib::Time t) const;
    /// True iff the clamp has per-expiry bands.
    bool perExpiry() const { return !bands_.empty(); }
    /// The time the inner local vol is read at for a simulation time `t`.
    QuantLib::Time clampedTime(QuantLib::Time t) const;

    /// Floor mode: the local vol is computed here with QuantLib's
    /// LocalVolSurface::localVolImpl formula, step by step (dupire()), on
    /// these handles, and max(Dupire, floor) is returned; where QuantLib
    /// would throw the floor is returned. Without flooring the value equals
    /// LocalVolSurface's bit for bit (tests/parity/mc_core_test.cpp).
    void setFloor(
        QuantLib::Real floor,
        QuantLib::Handle<QuantLib::BlackVolTermStructure> black,
        QuantLib::Handle<QuantLib::YieldTermStructure> riskFree,
        QuantLib::Handle<QuantLib::YieldTermStructure> dividend,
        QuantLib::Handle<QuantLib::Quote> spot);
    /// QuantLib's Dupire local vol at (t, level), NaN where QuantLib would
    /// throw (floor mode only).
    QuantLib::Real dupire(QuantLib::Time t, QuantLib::Real underlyingLevel) const;
    /// Floor mode counters since the last reset: reads, reads floored
    /// (refusals included) and refusals floored. Not thread-safe (one
    /// request, one simulation thread).
    std::size_t reads() const { return reads_; }
    std::size_t floored() const { return floored_; }
    std::size_t refusals() const { return refusals_; }
    void resetCounts() const { reads_ = floored_ = refusals_ = 0; }

    const QuantLib::Date& referenceDate() const override { return inner_->referenceDate(); }
    QuantLib::DayCounter dayCounter() const override { return inner_->dayCounter(); }
    QuantLib::Date maxDate() const override { return inner_->maxDate(); }
    QuantLib::Real minStrike() const override { return inner_->minStrike(); }
    QuantLib::Real maxStrike() const override { return inner_->maxStrike(); }

protected:
    QuantLib::Volatility localVolImpl(QuantLib::Time t, QuantLib::Real s) const override;

private:
    std::shared_ptr<QuantLib::LocalVolTermStructure> inner_;
    QuantLib::Real lower_, upper_;
    /// Per-expiry bands, margins applied (empty: lower_ / upper_).
    std::vector<LocalVolStrikeBand> bands_;
    /// Latest time read (QL_MAX_REAL: no time clamp).
    QuantLib::Time tMax_;
    std::optional<QuantLib::Real> floor_;
    QuantLib::Handle<QuantLib::BlackVolTermStructure> black_;
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree_, dividend_;
    QuantLib::Handle<QuantLib::Quote> spot_;
    /// The inner local vol is closed-form (SsviLocalVol): floor mode reads it
    /// instead of the finite-difference formula.
    bool analyticInner_ = false;
    mutable std::size_t reads_ = 0, floored_ = 0, refusals_ = 0;
};

/**
 * One edge-clamped LocalVolSurface per distinct (vol, spot, dividend,
 * riskFree) market,
 * keyed by the identity of the linked objects (a bumped spot quote, a
 * shifted vol or a shifted curve is another object and gets its own
 * surface, as it must: the local vol reads all four). `attach` sets
 * `asset.localVol` for a strike-dependent vol and leaves an exact-step
 * asset untouched. Scope: one request (the evaluator owns one per note).
 */
class LocalVolCache {
public:
    void attach(BasketAsset& asset);
    std::size_t size() const { return cache_.size(); }

private:
    using Key = std::tuple<
        const void*,
        const void*,
        const void*,
        const void*,
        int,
        QuantLib::Real,
        QuantLib::Real,
        std::vector<QuantLib::Real>,
        QuantLib::Real>;
    std::map<Key, QuantLib::Handle<QuantLib::LocalVolTermStructure>> cache_;
};

/**
 * Coefficients of the exact lognormal step of every asset over every step of
 * one time grid, so a path costs one exp per (asset, step) and no term
 * structure read. Per asset k and step s (t0 = grid[s], dt = grid.dt(s)), the
 * values QuantLib's evolve computes from the handles:
 *
 *   var   = vol.blackVariance(t0 + dt, 0.01) - vol.blackVariance(t0, 0.01)
 *   drift = (riskFree.forwardRate(t0, t0 + dt, Continuous, NoFrequency, true)
 *            - dividend.forwardRate(t0, t0 + dt, Continuous, NoFrequency, true))
 *           * dt - 0.5 * var
 *   stdDev = sqrt(var)
 *
 * (the strike passed to blackVariance is irrelevant for a strike-independent
 * vol, so this equals blackForwardVariance(t0, t0 + dt, spot)). The step is
 * then x[s + 1] = x[s] * exp(stdDev * dz + drift), dz = (sqrtCorrelation * dw)
 * of the step's Gaussian draws, in exactly that order of operations.
 */
struct LognormalStepTable {
    QuantLib::Size assets = 0;
    QuantLib::Size steps = 0;
    /// Row-major [step * assets + asset].
    std::vector<QuantLib::Real> drift;
    std::vector<QuantLib::Real> stdDev;
    /// The spectral pseudo-square-root of the correlation: the very matrix the
    /// StochasticProcessArray correlates its draws with.
    QuantLib::Matrix sqrtCorrelation;
    /// Spot of every asset at build time (the processes' initial values).
    std::vector<QuantLib::Real> x0;
};

/**
 * The basket's process array (one GeneralizedBlackScholesProcess per asset,
 * correlated by QuantLib's spectral pseudo-square-root) that also keeps the
 * assets it was built from, so that the path engine can build the exact-step
 * table of a time grid when every asset allows it. It is a
 * StochasticProcessArray in every other respect.
 */
class LognormalBasket : public QuantLib::StochasticProcessArray {
public:
    LognormalBasket(
        std::vector<BasketAsset> assets,
        const std::vector<std::shared_ptr<QuantLib::StochasticProcess1D>>& processes,
        const QuantLib::Matrix& correlation);

    const std::vector<BasketAsset>& assets() const { return assets_; }

    /// True iff every asset is exact-step capable (constant or term vol):
    /// stepTable() is then available and the paths of the table and of
    /// QuantLib's evolve coincide bit for bit.
    bool exactStep() const { return exactStep_; }

    /// The exact-step table over `grid` (grid.size() >= 2), read from the
    /// handles as they are linked now (a bumped spot, curve or vol is seen).
    /// QUANTRA_ERROR when !exactStep() (caller wiring bug) or the grid is too
    /// short. QuantLib exceptions from the curves and the vol propagate
    /// unchanged, exactly as they would from MultiPathGenerator.
    LognormalStepTable stepTable(const QuantLib::TimeGrid& grid) const;

private:
    std::vector<BasketAsset> assets_;
    bool exactStep_ = false;
};

class BasketModel {
public:
    /**
     * The process array of the basket: assets in the order given,
     * `correlation` (i, j) = correlation between the Brownian drivers of
     * assets i and j in that same order (CorrelationRegistry::subMatrix with
     * the asset ids gives exactly this). StochasticProcessArray takes the
     * spectral pseudo-square-root of the matrix, so a positive
     * semi-definite matrix (the registry's PSD check) is enough.
     *
     * 400 (QuantraInvalidArgument): no assets; a non-square correlation or
     * one whose dimension differs from assets.size(); a duplicate asset id;
     * a non-positive spot. QUANTRA_ERROR (500): an empty handle, or
     * `exactStepCapable` contradicting the vol's type (caller wiring bug).
     */
    static std::shared_ptr<LognormalBasket> build(
        const std::vector<BasketAsset>& assets, const QuantLib::Matrix& correlation);

    /// True iff every asset is exact-step capable (the time grid may then be
    /// the mandatory dates only).
    static bool allExactStepCapable(const std::vector<BasketAsset>& assets);
};

}} // namespace quantra::mc

#endif // QUANTRA_MC_BASKET_MODEL_H
