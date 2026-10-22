#include "path_engine.h"

#include "error.h"

#include <ql/math/randomnumbers/inversecumulativersg.hpp>
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/math/randomnumbers/sobolbrownianbridgersg.hpp>
#include <ql/math/randomnumbers/sobolrsg.hpp>
#include <ql/methods/montecarlo/multipathgenerator.hpp>

#include <cmath>
#include <limits>
#include <sstream>

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

template <class GSG>
PathEngineResult simulate(
    const std::shared_ptr<QuantLib::StochasticProcessArray>& processes,
    const QuantLib::TimeGrid& grid,
    GSG generator,
    const PathEngineConfig& config,
    QuantLib::Size nAccumulators,
    const PathFunctor& functor) {
    QuantLib::MultiPathGenerator<GSG> pathGenerator(processes, grid, std::move(generator), false);

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
            const QuantLib::MultiPath& path = pathGenerator.next().value;
            if (simulated < config.samplePaths) {
                result.samplePaths.push_back(copyPath(path));
            }
            ++simulated;
            std::fill(acc.begin(), acc.end(), 0.0);
            functor(path, acc);
        }
        if (config.antithetic) {
            const QuantLib::MultiPath& path = pathGenerator.antithetic().value;
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
