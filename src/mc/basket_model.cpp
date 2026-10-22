#include "basket_model.h"

#include "error.h"

#include <ql/processes/blackscholesprocess.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancecurve.hpp>

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

std::shared_ptr<QuantLib::StochasticProcessArray> BasketModel::build(
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
    return std::make_shared<QuantLib::StochasticProcessArray>(processes, correlation);
}

}} // namespace quantra::mc
