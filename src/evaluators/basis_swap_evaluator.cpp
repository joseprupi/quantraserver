#include "basis_swap_evaluator.h"

#include "error.h"

#include <cmath>
#include <iostream>

namespace quantra {

namespace {

constexpr double kBasisPoint = 1.0e-4;

BasisSwapPerSwap priceTrade(
    const BasisSwapTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    bool includeFlows) {
    const BuiltSwapLeg built1 = BuildSwapLeg(trade.leg1, "BasisSwap.leg1", reg, ctx);
    const BuiltSwapLeg built2 = BuildSwapLeg(trade.leg2, "BasisSwap.leg2", reg, ctx);

    BasisSwapPerSwap out;
    out.includeFlows = includeFlows;
    out.legs.push_back(SignedLegResult(trade.leg1, built1, ctx, includeFlows));
    out.legs.push_back(SignedLegResult(trade.leg2, built2, ctx, includeFlows));
    out.npv = out.legs[0].npv + out.legs[1].npv;

    // The fair spread is solved on the leg that carries the request's spread:
    // leg1 when it alone has one, leg2 otherwise (the quoted leg by
    // convention when both or neither carry one).
    const bool onLeg1 = trade.leg1.level() != 0.0 && trade.leg2.level() == 0.0;
    const SwapLegData& leg = onLeg1 ? trade.leg1 : trade.leg2;
    const double bps = SignedBps(leg, onLeg1 ? built1 : built2);
    if (std::fabs(bps) > 1e-16) {
        const double fairSpread = leg.level() - out.npv * kBasisPoint / bps;
        if (std::isfinite(fairSpread)) out.fairSpread = fairSpread;
    }

    std::cout << "Basis swap NPV (" << trade.leg1.currency << "): " << out.npv << std::endl;
    return out;
}

} // namespace

BasisSwapResult BasisSwapEvaluator::evaluate(
    const BasisSwapInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const {
    BasisSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check();
        result.swaps.push_back(priceTrade(trade, reg, ctx, inputs.includeFlows));
    }
    return result;
}

} // namespace quantra
