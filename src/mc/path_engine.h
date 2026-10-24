#ifndef QUANTRA_MC_PATH_ENGINE_H
#define QUANTRA_MC_PATH_ENGINE_H

/**
 * Path engine of the shared Monte Carlo core: simulates a process array
 * (basket_model.h) over a time grid (time_grid.h) and hands every path to a
 * caller-supplied functor that fills a vector of accumulators (discounted
 * cash flows, indicators, ...). The engine returns the mean and standard
 * error of every accumulator, the running means at 1/4, 1/2 and 3/4 of the
 * samples (convergence diagnostic) and, on request, the first simulated
 * paths. No product logic lives here.
 *
 * Two path sources, one result:
 *
 *   * a LognormalBasket whose assets are all exact-step capable (constant or
 *     term vols) is stepped from its LognormalStepTable: the Gaussian draws
 *     of the generator below, correlated with the basket's own spectral
 *     pseudo-square-root, applied as x * exp(stdDev * dz + drift) with the
 *     table's per-(asset, step) coefficients. This is QuantLib's
 *     GeneralizedBlackScholesProcess::evolve with its term-structure reads
 *     hoisted out of the path loop; the levels are bit-identical to
 *     MultiPathGenerator's (tests/parity/mc_core_test.cpp) at a fraction of
 *     the cost (about 28 times faster);
 *   * any other process array (a local-vol basket on a `steps_per_year`
 *     grid, or a plain StochasticProcessArray) goes through QuantLib's
 *     MultiPathGenerator and StochasticProcessArray::evolve.
 *
 * Random numbers (the Python parity test must reproduce the stream):
 *
 *   dimension = factors * steps, factors = number of assets, steps =
 *   grid.size() - 1. The Gaussian sequence is consumed TIME-MAJOR, as
 *   MultiPathGenerator does (ql/methods/montecarlo/multipathgenerator.hpp)
 *   and the table stepper reproduces: element (step - 1) * factors + asset
 *   drives asset `asset` over the step from grid[step - 1] to grid[step].
 *   The draws of a step are then correlated with the spectral
 *   pseudo-square-root of the correlation matrix (the one
 *   StochasticProcessArray computes).
 *
 *   Sequence::Sobol, brownianBridge = true (the default):
 *     SobolBrownianBridgeRsg(factors, steps, SobolBrownianGenerator::Diagonal,
 *                            seed, SobolRsg::JoeKuoD7)
 *     (ql/math/randomnumbers/sobolbrownianbridgersg.hpp): Sobol numbers,
 *     InverseCumulativeNormal, Brownian-bridge construction of each factor's
 *     increments with the Diagonal ordering of the Sobol dimensions. Its
 *     output is already the time-major vector of increments above, and it is
 *     consumed as the sequence generator (for MultiPathGenerator with its
 *     own brownianBridge flag false, QuantLib's MultiPathGenerator does not
 *     implement the bridge itself).
 *   Sequence::Sobol, brownianBridge = false:
 *     InverseCumulativeRsg<SobolRsg, InverseCumulativeNormal>(
 *         SobolRsg(dimension, seed, SobolRsg::JoeKuoD7))
 *     i.e. QuantLib's LowDiscrepancy traits with JoeKuoD7 direction integers
 *     (the bridge generator's table, 1898 tabulated dimensions) instead of
 *     the traits' Jaeckel default (32 dimensions). Sobol dimension k is
 *     consumed as (step = k / factors + 1, asset = k % factors).
 *   Sequence::PseudoRandom:
 *     PseudoRandom::make_sequence_generator(dimension, seed) =
 *     InverseCumulativeRsg<RandomSequenceGenerator<MersenneTwisterUniformRng>,
 *                          InverseCumulativeNormal>, Mersenne Twister seeded
 *     with `seed`. `brownianBridge` is accepted and NOT applied: the bridge
 *     is a reordering of low-discrepancy dimensions and changes nothing
 *     statistically for independent draws.
 *
 *   Sobol seed: QuantLib uses it only for direction integers beyond the
 *   tabulated 1898 dimensions (and seed 0 means clock-based there), so for
 *   smaller dimensions every seed gives the same stream.
 *
 * Antithetic variates: every path from the generator's next sequence is
 * followed by its antithetic (the same Gaussian draws negated). The functor runs
 * on both, their accumulator vectors are averaged and the PAIR is one sample
 * of the mean/standard-error statistics, so the reported standard error is
 * that of the antithetic estimator. `paths` counts simulated paths (functor
 * calls): with antithetic it must be even and samples = paths / 2.
 *
 * Standard error = sqrt(sample variance / samples) with the unbiased sample
 * variance over the samples. With Sobol it is the same sample statistic
 * (the points are not independent, so it is an indication, not a
 * confidence bound; the actual error of a low-discrepancy estimate is
 * usually far smaller).
 */

#include <ql/methods/montecarlo/multipath.hpp>
#include <ql/processes/stochasticprocessarray.hpp>
#include <ql/timegrid.hpp>
#include <ql/types.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace quantra { namespace mc {

struct PathEngineConfig {
    enum class Sequence { Sobol, PseudoRandom };

    /// Simulated paths (functor calls); > 0, even when antithetic.
    QuantLib::Size paths = 0;
    unsigned long seed = 42;
    Sequence sequence = Sequence::Sobol;
    bool antithetic = true;
    bool brownianBridge = true;
    /// Number of leading simulated paths returned in PathEngineResult::samplePaths
    /// (0 = none); must be <= paths.
    QuantLib::Size samplePaths = 0;
};

/// Running mean and standard error of every accumulator after `samples`
/// samples (the same estimator as the final one, on the first `samples`).
struct RunningMean {
    double fraction = 0.0;
    QuantLib::Size samples = 0;
    std::vector<QuantLib::Real> means;
    std::vector<QuantLib::Real> standardErrors;
};

struct PathEngineResult {
    /// Simulated paths (functor calls).
    QuantLib::Size paths = 0;
    /// Samples of the statistics: paths, or paths / 2 with antithetic.
    QuantLib::Size samples = 0;
    /// Per accumulator.
    std::vector<QuantLib::Real> mean;
    std::vector<QuantLib::Real> standardError;
    /// At 1/4, 1/2 and 3/4 of the samples (an entry is omitted when its
    /// sample count would be 0 or repeat the previous entry's).
    std::vector<RunningMean> runningMeans;
    /// The first config.samplePaths simulated paths, in simulation order
    /// (with antithetic: path, its antithetic, next path, ...), as
    /// [path][asset][grid point] levels (grid point 0 = spot).
    std::vector<std::vector<std::vector<QuantLib::Real>>> samplePaths;
};

/**
 * Per-path functor: `accumulators` arrives zeroed with `nAccumulators`
 * entries; the functor adds the path's contribution to each entry (e.g. the
 * discounted payoff of one bucket). The MultiPath is indexed [asset][grid
 * point], grid point 0 being the spot.
 */
using PathFunctor =
    std::function<void(const QuantLib::MultiPath&, std::vector<QuantLib::Real>& accumulators)>;

/**
 * Simulate `config.paths` paths of `processes` over `grid` and reduce the
 * functor's accumulators. A LognormalBasket (BasketModel::build) with every
 * asset exact-step capable is stepped from its exact-step table; anything
 * else through QuantLib's MultiPathGenerator.
 *
 * 400 (QuantraInvalidArgument): paths == 0; antithetic with an odd number of
 * paths; nAccumulators == 0; samplePaths > paths. QUANTRA_ERROR (500): a null
 * process array or a grid with fewer than two points. QuantLib exceptions
 * raised inside the processes (e.g. a local-vol surface that is not smooth
 * enough) propagate unchanged.
 */
PathEngineResult RunPaths(
    const std::shared_ptr<QuantLib::StochasticProcessArray>& processes,
    const QuantLib::TimeGrid& grid,
    const PathEngineConfig& config,
    QuantLib::Size nAccumulators,
    const PathFunctor& functor);

}} // namespace quantra::mc

#endif // QUANTRA_MC_PATH_ENGINE_H
