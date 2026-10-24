#include "path_engine.h"

#include "basket_model.h"
#include "error.h"

#include <ql/math/randomnumbers/inversecumulativersg.hpp>
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/math/randomnumbers/sobolbrownianbridgersg.hpp>
#include <ql/math/randomnumbers/sobolrsg.hpp>
#include <ql/methods/montecarlo/multipathgenerator.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <sstream>
#include <utility>

namespace quantra { namespace mc {

namespace {

using SobolBridgeGenerator = QuantLib::SobolBrownianBridgeRsg;
using SobolGenerator =
    QuantLib::InverseCumulativeRsg<QuantLib::SobolRsg, QuantLib::InverseCumulativeNormal>;
using PseudoRandomGenerator = QuantLib::PseudoRandom::rsg_type;

std::vector<std::vector<QuantLib::Real>> copyPath(const QuantLib::MultiPath& path) {
    std::vector<std::vector<QuantLib::Real>> out(path.assetNumber());
    for (QuantLib::Size j = 0; j < path.assetNumber(); ++j) {
        const auto& p = path[j];
        out[j].assign(p.begin(), p.end());
    }
    return out;
}

// Path source of the exact-step table (basket_model.h): the same Gaussian
// sequence generator as MultiPathGenerator<GSG> would consume, the same
// time-major reading of the draws, the same correlation matrix, and the step
// formula of GeneralizedBlackScholesProcess::evolve in the same order of
// operations, so that every level equals MultiPathGenerator's bit for bit
// (tests/parity/mc_core_test.cpp). The antithetic path negates the draws, as
// MultiPathGenerator::antithetic() does; negation commutes exactly with the
// correlation product.
template <class GSG> class TableStepper {
public:
    TableStepper(LognormalStepTable table, const QuantLib::TimeGrid& grid, GSG generator)
        : table_(std::move(table)), generator_(std::move(generator)), path_(table_.assets, grid) {}

    const QuantLib::MultiPath& next() { return step(generator_.nextSequence().value, false); }
    const QuantLib::MultiPath& antithetic() { return step(generator_.lastSequence().value, true); }

private:
    const QuantLib::MultiPath& step(const std::vector<QuantLib::Real>& draws, bool negate) {
        const QuantLib::Size n = table_.assets;
        for (QuantLib::Size k = 0; k < n; ++k) {
            path_[k].front() = table_.x0[k];
        }
        for (QuantLib::Size s = 0; s < table_.steps; ++s) {
            const QuantLib::Real* w = draws.data() + s * n;
            const QuantLib::Real* drift = table_.drift.data() + s * n;
            const QuantLib::Real* stdDev = table_.stdDev.data() + s * n;
            for (QuantLib::Size i = 0; i < n; ++i) {
                // Matrix * Array in QuantLib is std::inner_product(w, w + n,
                // row(i), 0.0): the sum runs over j from 0.0 as w[j] * c[i][j].
                QuantLib::Real z = 0.0;
                const QuantLib::Real* row = table_.sqrtCorrelation.row_begin(i);
                for (QuantLib::Size j = 0; j < n; ++j) {
                    z = z + w[j] * row[j];
                }
                if (negate) {
                    z = -z;
                }
                QuantLib::Path& p = path_[i];
                p[s + 1] = p[s] * std::exp(stdDev[i] * z + drift[i]);
            }
        }
        return path_;
    }

    LognormalStepTable table_;
    GSG generator_;
    QuantLib::MultiPath path_;
};

// QuantLib's own path source (local-vol baskets, or any process array that is
// not a LognormalBasket).
template <class GSG> class QuantLibStepper {
public:
    QuantLibStepper(
        const std::shared_ptr<QuantLib::StochasticProcessArray>& processes,
        const QuantLib::TimeGrid& grid,
        GSG generator)
        : generator_(processes, grid, std::move(generator), false) {}

    const QuantLib::MultiPath& next() { return generator_.next().value; }
    const QuantLib::MultiPath& antithetic() { return generator_.antithetic().value; }

private:
    QuantLib::MultiPathGenerator<GSG> generator_;
};

template <class Stepper>
PathEngineResult reduce(
    Stepper& stepper,
    const PathEngineConfig& config,
    QuantLib::Size nAccumulators,
    const PathFunctor& functor) {
    const QuantLib::Size samples = config.antithetic ? config.paths / 2 : config.paths;
    const QuantLib::Size quarter = samples / 4;
    const QuantLib::Size half = samples / 2;
    const QuantLib::Size threeQuarters = (3 * samples) / 4;

    PathEngineResult result;
    result.paths = config.paths;
    result.samples = samples;
    result.samplePaths.reserve(config.samplePaths);

    std::vector<QuantLib::Real> sum(nAccumulators, 0.0);
    std::vector<QuantLib::Real> sumSq(nAccumulators, 0.0);
    std::vector<QuantLib::Real> acc(nAccumulators, 0.0);
    std::vector<QuantLib::Real> accAntithetic(nAccumulators, 0.0);

    QuantLib::Size simulated = 0;
    auto maybeRecordRunningMean = [&](QuantLib::Size done, QuantLib::Size at, double fraction) {
        if (done != at || at == 0) {
            return;
        }
        if (!result.runningMeans.empty() && result.runningMeans.back().samples == at) {
            return;
        }
        RunningMean rm;
        rm.fraction = fraction;
        rm.samples = at;
        rm.means.resize(nAccumulators);
        rm.standardErrors.resize(nAccumulators);
        const auto n = static_cast<QuantLib::Real>(at);
        for (QuantLib::Size k = 0; k < nAccumulators; ++k) {
            const QuantLib::Real mean = sum[k] / n;
            rm.means[k] = mean;
            if (at < 2) {
                rm.standardErrors[k] = std::numeric_limits<QuantLib::Real>::quiet_NaN();
            } else {
                QuantLib::Real variance = (sumSq[k] - n * mean * mean) / (n - 1.0);
                if (variance < 0.0) variance = 0.0;
                rm.standardErrors[k] = std::sqrt(variance / n);
            }
        }
        result.runningMeans.push_back(std::move(rm));
    };

    for (QuantLib::Size s = 0; s < samples; ++s) {
        {
            const QuantLib::MultiPath& path = stepper.next();
            if (simulated < config.samplePaths) {
                result.samplePaths.push_back(copyPath(path));
            }
            ++simulated;
            std::fill(acc.begin(), acc.end(), 0.0);
            functor(path, acc);
        }
        if (config.antithetic) {
            const QuantLib::MultiPath& path = stepper.antithetic();
            if (simulated < config.samplePaths) {
                result.samplePaths.push_back(copyPath(path));
            }
            ++simulated;
            std::fill(accAntithetic.begin(), accAntithetic.end(), 0.0);
            functor(path, accAntithetic);
            for (QuantLib::Size k = 0; k < nAccumulators; ++k) {
                acc[k] = 0.5 * (acc[k] + accAntithetic[k]);
            }
        }
        for (QuantLib::Size k = 0; k < nAccumulators; ++k) {
            sum[k] += acc[k];
            sumSq[k] += acc[k] * acc[k];
        }
        const QuantLib::Size done = s + 1;
        maybeRecordRunningMean(done, quarter, 0.25);
        maybeRecordRunningMean(done, half, 0.5);
        maybeRecordRunningMean(done, threeQuarters, 0.75);
    }

    result.mean.resize(nAccumulators);
    result.standardError.resize(nAccumulators);
    const auto n = static_cast<QuantLib::Real>(samples);
    for (QuantLib::Size k = 0; k < nAccumulators; ++k) {
        const QuantLib::Real mean = sum[k] / n;
        result.mean[k] = mean;
        if (samples < 2) {
            result.standardError[k] = std::numeric_limits<QuantLib::Real>::quiet_NaN();
        } else {
            QuantLib::Real variance = (sumSq[k] - n * mean * mean) / (n - 1.0);
            if (variance < 0.0) {
                variance = 0.0; // rounding on a constant accumulator
            }
            result.standardError[k] = std::sqrt(variance / n);
        }
    }
    return result;
}

template <class GSG>
PathEngineResult simulate(
    const std::shared_ptr<QuantLib::StochasticProcessArray>& processes,
    const QuantLib::TimeGrid& grid,
    GSG generator,
    const PathEngineConfig& config,
    QuantLib::Size nAccumulators,
    const PathFunctor& functor) {
    const auto basket = std::dynamic_pointer_cast<const LognormalBasket>(processes);
    if (basket && basket->exactStep()) {
        TableStepper<GSG> stepper(basket->stepTable(grid), grid, std::move(generator));
        return reduce(stepper, config, nAccumulators, functor);
    }
    QuantLibStepper<GSG> stepper(processes, grid, std::move(generator));
    return reduce(stepper, config, nAccumulators, functor);
}

} // namespace

PathEngineResult RunPaths(
    const std::shared_ptr<QuantLib::StochasticProcessArray>& processes,
    const QuantLib::TimeGrid& grid,
    const PathEngineConfig& config,
    QuantLib::Size nAccumulators,
    const PathFunctor& functor) {
    if (!processes) {
        QUANTRA_ERROR("path engine: process array is null");
    }
    if (grid.size() < 2) {
        QUANTRA_ERROR("path engine: time grid needs at least one step");
    }
    if (config.paths == 0) {
        QUANTRA_INVALID_ARGUMENT("path engine: paths must be > 0");
    }
    if (config.antithetic && config.paths % 2 != 0) {
        std::ostringstream msg;
        msg << "path engine: paths must be even with antithetic variates, got " << config.paths;
        QUANTRA_INVALID_ARGUMENT(msg.str());
    }
    if (nAccumulators == 0) {
        QUANTRA_INVALID_ARGUMENT("path engine: at least one accumulator is required");
    }
    if (config.samplePaths > config.paths) {
        std::ostringstream msg;
        msg << "path engine: sample_paths (" << config.samplePaths << ") must be <= paths ("
            << config.paths << ")";
        QUANTRA_INVALID_ARGUMENT(msg.str());
    }
    if (!functor) {
        QUANTRA_ERROR("path engine: path functor is empty");
    }

    const QuantLib::Size factors = processes->factors();
    const QuantLib::Size steps = grid.size() - 1;
    const QuantLib::Size dimension = factors * steps;

    switch (config.sequence) {
        case PathEngineConfig::Sequence::Sobol:
            if (config.brownianBridge) {
                return simulate(
                    processes, grid,
                    SobolBridgeGenerator(
                        factors, steps, QuantLib::SobolBrownianGenerator::Diagonal, config.seed,
                        QuantLib::SobolRsg::JoeKuoD7),
                    config, nAccumulators, functor);
            }
            return simulate(
                processes, grid,
                SobolGenerator(
                    QuantLib::SobolRsg(dimension, config.seed, QuantLib::SobolRsg::JoeKuoD7)),
                config, nAccumulators, functor);
        case PathEngineConfig::Sequence::PseudoRandom:
            return simulate(
                processes, grid,
                QuantLib::PseudoRandom::make_sequence_generator(dimension, config.seed), config,
                nAccumulators, functor);
    }
    QUANTRA_ERROR("path engine: unknown random sequence");
    return {};
}

}} // namespace quantra::mc
