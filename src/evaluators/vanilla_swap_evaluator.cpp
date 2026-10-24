#include "vanilla_swap_evaluator.h"

#include "error.h"

#include <cmath>

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
    // VanillaSwap::fairSpread: spread - NPV / (floatingLegBPS / basisPoint),
    // the same linear solve on the floating leg's signed annuity (the IBOR
    // forwards or CMS coupons unchanged; a capped / floored CMS leg keeps its
    // strikes, so there the result is the first-order fair spread).
    const double floatingBps = SignedBps(trade.floatingLeg, floating);
    if (floatingBps != 0.0) {
        const double fairSpread = trade.floatingLeg.level() - out.npv / (floatingBps / kBasisPoint);
        if (std::isfinite(fairSpread)) out.fairSpread = fairSpread;
    }
    out.cmsUsed = floating.cmsUsed;
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
