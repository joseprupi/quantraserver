#include "callable_fixed_rate_bond_evaluator.h"

#include <unordered_map>
#include <variant>

#include <ql/experimental/callablebonds/treecallablebondengine.hpp>
#include <ql/handle.hpp>
#include <ql/models/shortrate/onefactormodels/hullwhite.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "date_convert.h"
#include "enums_domain.h"
#include "error.h"
#include "model_domain.h"
#include "swaption_model_calibration.h"

namespace quantra {

CallableFixedRateBondResult CallableFixedRateBondEvaluator::evaluate(
    const CallableFixedRateBondInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {

    CallableFixedRateBondResult result;
    result.bonds.reserve(inputs.trades.size());

    // HW calibrations are deterministic for a given (model_id, asOf, market),
    // so calibrate at most once per model_id per request (the upstream
    // HwCalibrateCache also fronts this across requests). Mirrors the swaption
    // evaluator's per-request cache.
    std::unordered_map<std::string, HwCalibResult> hwCalibrationCache;

    for (const auto& trade : inputs.trades) {
        ctx.budget.check();  // honor the per-request deadline before each trade

        if (trade.bond == nullptr) {
            QUANTRA_INVALID_ARGUMENT("Callable bond instrument is null");
        }

        auto curveIt = reg.rates.curves.find(trade.discountingCurveId);
        if (curveIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
        }
        const auto& curveHandle = *curveIt->second;               // RelinkableHandle
        QuantLib::Handle<QuantLib::YieldTermStructure> discountHandle(curveHandle.currentLink());

        auto mIt = reg.volatility.modelDomains.find(trade.modelId);
        if (mIt == reg.volatility.modelDomains.end()) {
            QUANTRA_NOT_FOUND("Model not found: " + trade.modelId);
        }
        const auto* modelDomain = std::get_if<SwaptionModelDomain>(&mIt->second.payload);
        if (modelDomain == nullptr) {
            QUANTRA_INVALID_ARGUMENT(
                "Model '" + trade.modelId + "' is not a short-rate (SwaptionModelSpec) model");
        }
        if (modelDomain->model_type != IrModelTypeKind::HullWhiteLattice) {
            QUANTRA_INVALID_ARGUMENT(
                "Model '" + trade.modelId + "': callable bonds require a HullWhiteLattice model");
        }

        // Resolve the Hull-White parameters: explicit or (cached) calibration.
        double a = modelDomain->hw_a;
        double sigma = modelDomain->hw_sigma;
        if (modelDomain->param_mode == ModelParamModeKind::Calibrate) {
            if (!modelDomain->hw_calibration) {
                QUANTRA_INVALID_ARGUMENT(
                    "Model '" + trade.modelId + "' param_mode=Calibrate requires hw_calibration");
            }
            auto cacheIt = hwCalibrationCache.find(trade.modelId);
            if (cacheIt == hwCalibrationCache.end()) {
                cacheIt = hwCalibrationCache.emplace(
                    trade.modelId,
                    calibrateHullWhiteFromSwaptionVol(reg, *modelDomain->hw_calibration, ctx.asOf))
                    .first;
            }
            a = cacheIt->second.a;
            sigma = cacheIt->second.sigma;
        }

        if (trade.treeSteps < kMinTreeSteps) {
            QUANTRA_INVALID_ARGUMENT("Callable bond tree_steps must be >= 1");
        }

        // Same wiring the QuantLib CallableBonds example uses: the Hull-White
        // model and the tree engine both carry the discount term structure.
        auto hwModel = QuantLib::ext::make_shared<QuantLib::HullWhite>(discountHandle, a, sigma);
        auto engine = QuantLib::ext::make_shared<QuantLib::TreeCallableFixedRateBondEngine>(
            hwModel, static_cast<QuantLib::Size>(trade.treeSteps), discountHandle);
        trade.bond->setPricingEngine(engine);

        CallableFixedRateBondPerBond out;
        out.npv = trade.bond->NPV();
        out.cleanPrice = trade.bond->cleanPrice();
        out.dirtyPrice = trade.bond->dirtyPrice();
        out.settlementDate = DateToIso(trade.bond->settlementDate());
        result.bonds.push_back(std::move(out));
    }

    return result;
}

} // namespace quantra
