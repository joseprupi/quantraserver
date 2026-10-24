#include "ois_swap_evaluator.h"

#include "error.h"

#include <cmath>

namespace quantra {

namespace {

constexpr double kBasisPoint = 1.0e-4;

OisSwapPerSwap priceTrade(
    const OisSwapTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    bool includeFlows) {
    const BuiltSwapLeg fixed = BuildSwapLeg(trade.fixedLeg, "OisSwap.fixed_leg", reg, ctx);
    const BuiltSwapLeg overnight =
        BuildSwapLeg(trade.overnightLeg, "OisSwap.overnight_leg", reg, ctx);

    OisSwapPerSwap out;
    out.includeFlows = includeFlows;
    out.legs.push_back(SignedLegResult(trade.fixedLeg, fixed, ctx, includeFlows));
    out.legs.push_back(SignedLegResult(trade.overnightLeg, overnight, ctx, includeFlows));
    out.npv = out.legs[0].npv + out.legs[1].npv;

    // OvernightIndexedSwap::fairRate: fixedRate - NPV / (fixedLegBPS /
    // basisPoint), the leg BPS carrying the payer sign.
    const double fixedBps = SignedBps(trade.fixedLeg, fixed);
    if (fixedBps != 0.0) {
        const double fairRate = trade.fixedLeg.level() - out.npv / (fixedBps / kBasisPoint);
        if (std::isfinite(fairRate)) out.fairRate = fairRate;
    }
    // OvernightIndexedSwap::fairSpread: spread - NPV / (overnightLegBPS /
    // basisPoint), the same linear solve on the overnight leg's signed annuity.
    const double overnightBps = SignedBps(trade.overnightLeg, overnight);
    if (overnightBps != 0.0) {
        const double fairSpread =
            trade.overnightLeg.level() - out.npv / (overnightBps / kBasisPoint);
        if (std::isfinite(fairSpread)) out.fairSpread = fairSpread;
    }
    return out;
}

} // namespace

OisSwapResult OisSwapEvaluator::evaluate(
    const OisSwapInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const {
    OisSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check();
        result.swaps.push_back(priceTrade(trade, reg, ctx, inputs.includeFlows));
    }
    return result;
}

} // namespace quantra
