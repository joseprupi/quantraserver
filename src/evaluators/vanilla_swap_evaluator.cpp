#include "vanilla_swap_evaluator.h"

#include "error.h"

#include <cmath>
#include <iostream>

namespace quantra {

namespace {

constexpr double kBasisPoint = 1.0e-4;

VanillaSwapPerSwap priceTrade(
    const VanillaSwapTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    bool includeFlows) {
    const BuiltSwapLeg fixed = BuildSwapLeg(trade.fixedLeg, "VanillaSwap.fixed_leg", reg, ctx);
    const BuiltSwapLeg floating =
        BuildSwapLeg(trade.floatingLeg, "VanillaSwap.floating_leg", reg, ctx);

    VanillaSwapPerSwap out;
    out.includeFlows = includeFlows;
    out.legs.push_back(SignedLegResult(trade.fixedLeg, fixed, ctx, includeFlows));
    out.legs.push_back(SignedLegResult(trade.floatingLeg, floating, ctx, includeFlows));
    out.npv = out.legs[0].npv + out.legs[1].npv;

    // VanillaSwap::fairRate: fixedRate - NPV / (fixedLegBPS / basisPoint),
    // the leg BPS carrying the payer sign. A zero annuity has no fair rate.
    const double fixedBps = SignedBps(trade.fixedLeg, fixed);
    if (fixedBps != 0.0) {
        const double fairRate = trade.fixedLeg.level() - out.npv / (fixedBps / kBasisPoint);
        if (std::isfinite(fairRate)) out.fairRate = fairRate;
    }
    out.cmsUsed = floating.cmsUsed;

    std::cout << "Vanilla swap NPV (" << trade.fixedLeg.currency << "): " << out.npv << std::endl;
    return out;
}

} // namespace

VanillaSwapResult VanillaSwapEvaluator::evaluate(
    const VanillaSwapInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const {
    VanillaSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check(); // honor the per-request deadline before each trade
        result.swaps.push_back(priceTrade(trade, reg, ctx, inputs.includeFlows));
    }
    return result;
}

} // namespace quantra
