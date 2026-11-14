#include "commodity_swap_evaluator.h"

#include "error.h"

#include <string>
#include <variant>

namespace quantra {

namespace {

/// The library leg with its contract / spot price and discount curve
/// resolved from the registry; an unknown id is a 404 naming `path` (the leg,
/// e.g. `swaps[2].commodity_swap.leg1`) and the field.
commodities::CommoditySwapLeg resolveLeg(
    const CommoditySwapLegSpec& spec, const PricingRegistry& reg, const std::string& path) {
    commodities::CommoditySwapLeg leg = spec.leg;
    if (auto* futures = std::get_if<commodities::FuturesFloatingPrice>(&leg.price)) {
        futures->contract = reg.commodity.contract(spec.contractId, path + ".price.contract_id");
    } else if (auto* spot = std::get_if<commodities::SpotFloatingPrice>(&leg.price)) {
        spot->spot = reg.commodity.spotPrice(spec.spotId, path + ".price.spot_id");
    }
    auto curve = reg.rates.curves.find(spec.discountingCurveId);
    if (curve == reg.rates.curves.end() || !curve->second) {
        QUANTRA_NOT_FOUND(
            path + ".discounting_curve: discounting curve not found: " + spec.discountingCurveId);
    }
    leg.discountCurve = QuantLib::Handle<QuantLib::YieldTermStructure>(*curve->second);
    return leg;
}

} // namespace

CommoditySwapResultSet CommoditySwapEvaluator::evaluate(
    const CommoditySwapInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    CommoditySwapResultSet result;
    result.includeFlows = inputs.includeFlows;
    result.includeObservations = inputs.includeObservations;
    result.swaps.reserve(inputs.trades.size());
    for (std::size_t i = 0; i < inputs.trades.size(); ++i) {
        ctx.budget.check();
        const auto& trade = inputs.trades[i];
        const std::string path = "swaps[" + std::to_string(i) + "].commodity_swap";
        const auto leg1 = resolveLeg(trade.legs[0], reg, path + ".leg1");
        const auto leg2 = resolveLeg(trade.legs[1], reg, path + ".leg2");
        try {
            result.swaps.push_back(commodities::PriceCommoditySwap(leg1, leg2, ctx.asOf));
        } catch (const QuantraInvalidArgument& e) {
            // The library names the field relative to the swap, or the market
            // data it read; add which swap.
            QUANTRA_INVALID_ARGUMENT(commodities::QualifySwapMessage(path, e.what()));
        }
    }
    return result;
}

} // namespace quantra
