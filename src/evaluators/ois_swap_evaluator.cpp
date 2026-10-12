#include "ois_swap_evaluator.h"

#include "date_convert.h"
#include "error.h"
#include "swap_leg_builder.h"

#include <ql/cashflow.hpp>
#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>

#include <iostream>
#include <limits>

namespace quantra {

namespace {

/**
 * Pre-serialize a single OIS swap cash flow to a plain flow record. Mirrors
 * the original swap-leg flow shape verbatim, so the mapper can emit
 * SwapLegFlow offsets without ever touching QuantLib types. Coupons that
 * have already occurred relative to ctx.asOf are skipped (legacy behaviour).
 */
void extractFlows(
    const QuantLib::Leg& leg,
    const QuantLib::YieldTermStructure& discountCurve,
    const QuantLib::Date& asOf,
    std::vector<OisSwapFlowPlain>& out) {
    for (const auto& cf : leg) {
        if (!cf || cf->hasOccurred(asOf)) {
            continue;
        }
        OisSwapFlowPlain f;
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

OisSwapPerSwap priceTrade(
    const OisSwapTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    bool includeFlows) {
    auto discIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }
    auto fwdIt = reg.rates.curves.find(trade.forwardingCurveId);
    if (fwdIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Forwarding curve not found: " + trade.forwardingCurveId);
    }
    if (!reg.rates.indices.has(trade.overnight.indexId)) {
        QUANTRA_NOT_FOUND("Index not found: " + trade.overnight.indexId);
    }

    const auto& discHandle = *discIt->second;
    const auto& fwdHandle = *fwdIt->second;

    // IndexRegistry::getOvernightWithCurve downcasts the registry entry and
    // raises if the requested index is not an OvernightIndex.
    auto overnightIndex = reg.rates.indices.getOvernightWithCurve(
        trade.overnight.indexId,
        QuantLib::Handle<QuantLib::YieldTermStructure>(fwdHandle.currentLink()));

    if (trade.fixed.notional != trade.overnight.notional) {
        // Mismatched notionals are not an error: the fixed leg's notional wins
        // on both legs (QuantLib::OvernightIndexedSwap takes one nominal).
        std::cout << "Warning: Fixed and overnight notionals differ. Using fixed notional."
                  << std::endl;
    }

    // Both legs come from the shared builder (swap_leg_builder.h), with the
    // chains QuantLib::OvernightIndexedSwap uses: the fixed leg pays with the
    // overnight leg's payment lag / calendar / convention, the overnight leg
    // carries the averaging, lookback, lockout and observation-shift
    // conventions. `price-swap` builds an OIS leg through the same function,
    // which is what makes the two endpoints agree bit-for-bit.
    QuantLib::Leg fixedLeg = BuildFixedRateLeg(
        FixedRateLegSpec{
            trade.fixed.schedule, std::vector<double>{trade.fixed.notional}, trade.fixed.rate,
            trade.fixed.dayCounter, trade.overnight.paymentConvention,
            trade.overnight.paymentCalendar, trade.overnight.paymentLag});
    QuantLib::Leg overnightLeg = BuildOvernightLeg(
        OvernightLegSpec{
            trade.overnight.schedule, std::vector<double>{trade.fixed.notional},
            trade.overnight.spread, trade.overnight.paymentConvention,
            trade.overnight.paymentCalendar, trade.overnight.paymentLag,
            trade.overnight.averagingMethod, trade.overnight.lookbackDays,
            trade.overnight.lockoutDays, trade.overnight.applyObservationShift,
            trade.overnight.telescopicValueDates},
        overnightIndex);

    // Payer = pays the fixed leg. Priced as a generic QuantLib::Swap on the
    // DiscountingSwapEngine, exactly as OvernightIndexedSwap (a Swap) is.
    const bool payerFixed = (trade.swapType == QuantLib::OvernightIndexedSwap::Payer);
    std::vector<QuantLib::Leg> legs{fixedLeg, overnightLeg};
    std::vector<bool> payer{payerFixed, !payerFixed};
    auto swap = std::make_shared<QuantLib::Swap>(legs, payer);
    swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discHandle));

    OisSwapPerSwap out;
    out.npv = swap->NPV();
    out.fixedLegBps = swap->legBPS(0);
    out.overnightLegBps = swap->legBPS(1);
    out.fixedLegNpv = swap->legNPV(0);
    out.overnightLegNpv = swap->legNPV(1);
    // OvernightIndexedSwap's own formulas (basisPoint = 1e-4), the leg BPS
    // already carrying the payer sign:
    //   fairRate   = fixedRate - NPV / (fixedLegBPS / basisPoint)
    //   fairSpread = spread    - NPV / (overnightLegBPS / basisPoint)
    constexpr double basisPoint = 1.0e-4;
    out.fairRate = trade.fixed.rate - out.npv / (out.fixedLegBps / basisPoint);
    out.fairSpread = trade.overnight.spread - out.npv / (out.overnightLegBps / basisPoint);
    out.includeFlows = includeFlows;

    if (includeFlows) {
        auto discountCurve = discHandle.currentLink();
        extractFlows(fixedLeg, *discountCurve, ctx.asOf, out.fixedFlows);
        extractFlows(overnightLeg, *discountCurve, ctx.asOf, out.overnightFlows);
    }
    return out;
}

} // namespace

OisSwapResult OisSwapEvaluator::evaluate(
    const OisSwapInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const {
    OisSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        result.swaps.push_back(priceTrade(trade, reg, ctx, inputs.includeFlows));
    }
    return result;
}

} // namespace quantra
