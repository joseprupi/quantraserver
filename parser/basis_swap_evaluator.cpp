#include "basis_swap_evaluator.h"

#include <cmath>
#include <limits>

#include <ql/cashflow.hpp>
#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>

#include "common.h"
#include "error.h"

namespace quantra {

namespace {

/**
 * Pre-serialize a single basis swap cash flow to a plain flow record. Mirrors
 * the legacy swap_leg_flow_builder shape verbatim, so the mapper can emit
 * SwapLegFlow offsets without ever touching QuantLib types. Coupons that have
 * already occurred relative to ctx.asOf are skipped (legacy behaviour).
 */
void extractFlows(
    const QuantLib::Leg& leg,
    const QuantLib::YieldTermStructure& discountCurve,
    const QuantLib::Date& asOf,
    std::vector<BasisSwapFlowPlain>& out) {
    for (const auto& cf : leg) {
        if (!cf || cf->hasOccurred(asOf)) {
            continue;
        }
        BasisSwapFlowPlain f;
        f.paymentDate = DateToIso(cf->date());
        f.amount = cf->amount();
        f.discount = discountCurve.discount(cf->date());
        f.presentValue = f.amount * f.discount;

        auto coupon = std::dynamic_pointer_cast<QuantLib::Coupon>(cf);
        if (coupon) {
            f.accrualStartDate = DateToIso(coupon->accrualStartDate());
            f.accrualEndDate = DateToIso(coupon->accrualEndDate());
            f.accrualYearFraction = coupon->accrualPeriod();
            f.rate = coupon->rate();
        }

        auto frc = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(cf);
        if (frc) {
            f.isFloating = true;
            f.fixingDate = DateToIso(frc->fixingDate());
            f.indexFixing = frc->indexFixing();
            f.spread = frc->spread();
        }
        out.push_back(std::move(f));
    }
}

/// Build a QuantLib IborLeg from a plain BasisSwapFloatingLegData and an
/// already-cloned IborIndex (with the desired forwarding curve attached).
QuantLib::Leg buildIborLeg(const BasisSwapFloatingLegData& leg,
                           const std::shared_ptr<QuantLib::IborIndex>& index) {
    return QuantLib::IborLeg(leg.schedule, index)
        .withNotionals(leg.notional)
        .withPaymentDayCounter(leg.dayCounter)
        .withPaymentAdjustment(leg.paymentConvention)
        .withSpreads(leg.spread)
        .withFixingDays(leg.fixingDays)
        .inArrears(leg.inArrears);
}

BasisSwapPerSwap priceTrade(const BasisSwapTrade& trade,
                            const PricingRegistry& reg,
                            const PricingContext& ctx,
                            bool includeFlows) {
    auto discIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }
    auto fwd1It = reg.rates.curves.find(trade.forwardingCurveLeg1Id);
    if (fwd1It == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Forwarding curve leg1 not found: " + trade.forwardingCurveLeg1Id);
    }
    auto fwd2It = reg.rates.curves.find(trade.forwardingCurveLeg2Id);
    if (fwd2It == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Forwarding curve leg2 not found: " + trade.forwardingCurveLeg2Id);
    }
    if (!reg.rates.indices.has(trade.leg1.indexId)) {
        QUANTRA_NOT_FOUND("Index not found: " + trade.leg1.indexId);
    }
    if (!reg.rates.indices.has(trade.leg2.indexId)) {
        QUANTRA_NOT_FOUND("Index not found: " + trade.leg2.indexId);
    }

    const auto& discHandle = *discIt->second;
    const auto& fwd1Handle = *fwd1It->second;
    const auto& fwd2Handle = *fwd2It->second;

    // IndexRegistry::getIborWithCurve downcasts the registry entry to
    // IborIndex and raises if the requested index is not an IborIndex.
    auto idx1 = reg.rates.indices.getIborWithCurve(
        trade.leg1.indexId,
        QuantLib::Handle<QuantLib::YieldTermStructure>(fwd1Handle.currentLink()));
    auto idx2 = reg.rates.indices.getIborWithCurve(
        trade.leg2.indexId,
        QuantLib::Handle<QuantLib::YieldTermStructure>(fwd2Handle.currentLink()));

    QuantLib::Leg qlLeg1 = buildIborLeg(trade.leg1, idx1);
    QuantLib::Leg qlLeg2 = buildIborLeg(trade.leg2, idx2);

    // Payer = pay leg1 / receive leg2. Receiver = receive leg1 / pay leg2.
    const bool payerLeg1 = (trade.swapType == QuantLib::VanillaSwap::Payer);
    std::vector<QuantLib::Leg> legs{qlLeg1, qlLeg2};
    std::vector<bool> payer{payerLeg1, !payerLeg1};

    auto swap = std::make_shared<QuantLib::Swap>(legs, payer);
    swap->setPricingEngine(
        std::make_shared<QuantLib::DiscountingSwapEngine>(discHandle));

    BasisSwapPerSwap out;
    out.npv = swap->NPV();
    out.leg1Bps = swap->legBPS(0);
    out.leg2Bps = swap->legBPS(1);
    out.leg1Npv = swap->legNPV(0);
    out.leg2Npv = swap->legNPV(1);

    const double currentSpread1 = trade.leg1.spread;
    const double currentSpread2 = trade.leg2.spread;
    out.fairSpreadLeg1 = std::fabs(out.leg1Bps) > 1e-16
        ? (currentSpread1 - out.npv * 1e-4 / out.leg1Bps)
        : std::numeric_limits<double>::quiet_NaN();
    out.fairSpreadLeg2 = std::fabs(out.leg2Bps) > 1e-16
        ? (currentSpread2 - out.npv * 1e-4 / out.leg2Bps)
        : std::numeric_limits<double>::quiet_NaN();
    out.includeFlows = includeFlows;

    if (includeFlows) {
        auto discountCurve = discHandle.currentLink();
        extractFlows(qlLeg1, *discountCurve, ctx.asOf, out.leg1Flows);
        extractFlows(qlLeg2, *discountCurve, ctx.asOf, out.leg2Flows);
    }
    return out;
}

} // namespace

BasisSwapResult BasisSwapEvaluator::evaluate(const BasisSwapInputs& inputs,
                                       const PricingRegistry& reg,
                                       const PricingContext& ctx) const {
    BasisSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        result.swaps.push_back(priceTrade(trade, reg, ctx, inputs.includeFlows));
    }
    return result;
}

} // namespace quantra
