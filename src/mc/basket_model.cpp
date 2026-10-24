#include "basket_model.h"

#include "error.h"

#include <ql/interestrate.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancecurve.hpp>

#include <cmath>
#include <set>
#include <sstream>

namespace quantra { namespace mc {

bool IsExactStepCapable(const QuantLib::Handle<QuantLib::BlackVolTermStructure>& vol) {
    if (vol.empty()) {
        QUANTRA_ERROR("basket model: Black vol handle must be linked");
    }
    // Mirrors GeneralizedBlackScholesProcess::localVolatility(): a
    // BlackConstantVol or a BlackVarianceCurve is strike-independent and gets
    // the exact lognormal step; anything else goes through LocalVolSurface.
    const auto& ts = vol.currentLink();
    if (std::dynamic_pointer_cast<QuantLib::BlackConstantVol>(ts)) {
        return true;
    }
    if (std::dynamic_pointer_cast<QuantLib::BlackVarianceCurve>(ts)) {
        return true;
    }
    return false;
}

BasketAsset MakeBasketAsset(
    const std::string& id,
    const QuantLib::Handle<QuantLib::Quote>& spot,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree,
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& vol) {
    BasketAsset asset;
    asset.id = id;
    asset.spot = spot;
    asset.dividend = dividend;
    asset.riskFree = riskFree;
    asset.vol = vol;
    asset.exactStepCapable = IsExactStepCapable(vol);
    return asset;
}

bool BasketModel::allExactStepCapable(const std::vector<BasketAsset>& assets) {
    for (const auto& a : assets) {
        if (!a.exactStepCapable) {
            return false;
        }
    }
    return true;
}

LognormalBasket::LognormalBasket(
    std::vector<BasketAsset> assets,
    const std::vector<std::shared_ptr<QuantLib::StochasticProcess1D>>& processes,
    const QuantLib::Matrix& correlation)
    : QuantLib::StochasticProcessArray(processes, correlation), assets_(std::move(assets)),
      exactStep_(BasketModel::allExactStepCapable(assets_)) {}

LognormalStepTable LognormalBasket::stepTable(const QuantLib::TimeGrid& grid) const {
    if (!exactStep_) {
        QUANTRA_ERROR("basket model: the exact-step table needs every asset exact-step capable");
    }
    if (grid.size() < 2) {
        QUANTRA_ERROR("basket model: the exact-step table needs a grid with at least one step");
    }
    using QuantLib::Continuous;
    using QuantLib::NoFrequency;
    using QuantLib::Real;
    using QuantLib::Size;
    using QuantLib::Time;

    LognormalStepTable table;
    table.assets = assets_.size();
    table.steps = grid.size() - 1;
    table.drift.assign(table.assets * table.steps, 0.0);
    table.stdDev.assign(table.assets * table.steps, 0.0);
    // The matrix StochasticProcessArray correlates the draws with (its
    // constructor's pseudoSqrt(correlation, SalvagingAlgorithm::Spectral)).
    table.sqrtCorrelation = sqrtCorrelation_;
    table.x0.reserve(table.assets);
    for (const auto& a : assets_) {
        table.x0.push_back(a.spot->value());
    }
    for (Size s = 0; s < table.steps; ++s) {
        // MultiPathGenerator::next passes t0 = grid[s] and dt = grid.dt(s) to
        // evolve, which reads the term structures at t0 and t0 + dt (not at
        // grid[s + 1], which can differ in the last bit).
        const Time t0 = grid[s];
        const Time dt = grid.dt(s);
        for (Size k = 0; k < table.assets; ++k) {
            const auto& a = assets_[k];
            // Same reads, same expression, same order as
            // GeneralizedBlackScholesProcess::evolve for a strike-independent
            // vol (ql/processes/blackscholesprocess.cpp, QuantLib 1.44).
            const Real var = a.vol->blackVariance(t0 + dt, 0.01) - a.vol->blackVariance(t0, 0.01);
            const Real drift =
                (a.riskFree->forwardRate(t0, t0 + dt, Continuous, NoFrequency, true).rate() -
                 a.dividend->forwardRate(t0, t0 + dt, Continuous, NoFrequency, true).rate()) *
                    dt -
                0.5 * var;
            table.drift[s * table.assets + k] = drift;
            table.stdDev[s * table.assets + k] = std::sqrt(var);
        }
    }
    return table;
}

std::shared_ptr<LognormalBasket> BasketModel::build(
    const std::vector<BasketAsset>& assets, const QuantLib::Matrix& correlation) {
    if (assets.empty()) {
        QUANTRA_INVALID_ARGUMENT("basket model: at least one asset is required");
    }
    if (correlation.rows() != correlation.columns()) {
        std::ostringstream msg;
        msg << "basket model: correlation matrix must be square, got " << correlation.rows() << "x"
            << correlation.columns();
        QUANTRA_INVALID_ARGUMENT(msg.str());
    }
    if (correlation.rows() != assets.size()) {
        std::ostringstream msg;
        msg << "basket model: correlation matrix dimension " << correlation.rows()
            << " does not match the number of assets " << assets.size();
        QUANTRA_INVALID_ARGUMENT(msg.str());
    }

    std::set<std::string> ids;
    std::vector<std::shared_ptr<QuantLib::StochasticProcess1D>> processes;
    processes.reserve(assets.size());
    for (const auto& a : assets) {
        if (!ids.insert(a.id).second) {
            QUANTRA_INVALID_ARGUMENT("basket model: duplicate asset id: " + a.id);
        }
        if (a.spot.empty() || a.dividend.empty() || a.riskFree.empty() || a.vol.empty()) {
            QUANTRA_ERROR(
                "basket model: every spot, curve and vol handle must be linked for asset: " + a.id);
        }
        if (!(a.spot->value() > 0.0)) {
            QUANTRA_INVALID_ARGUMENT("basket model: spot must be positive for asset: " + a.id);
        }
        if (a.exactStepCapable != IsExactStepCapable(a.vol)) {
            QUANTRA_ERROR(
                "basket model: exactStepCapable does not match the vol type for asset: " + a.id);
        }
        processes.push_back(
            std::make_shared<QuantLib::GeneralizedBlackScholesProcess>(
                a.spot, a.dividend, a.riskFree, a.vol));
    }
    return std::make_shared<LognormalBasket>(assets, processes, correlation);
}

}} // namespace quantra::mc
