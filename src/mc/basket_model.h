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
 * QuantLib only: no FlatBuffers, no gRPC, no registries in this module.
 */

#include <ql/handle.hpp>
#include <ql/math/matrix.hpp>
#include <ql/processes/stochasticprocessarray.hpp>
#include <ql/quote.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/timegrid.hpp>

#include <memory>
#include <string>
#include <vector>

namespace quantra { namespace mc {

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
