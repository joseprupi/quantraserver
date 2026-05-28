#include "ois_swap_pricer.h"

#include <iostream>
#include <limits>

#include <ql/cashflow.hpp>
#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/utilities/null.hpp>

#include "common.h"
#include "error.h"

namespace quantra {

namespace {

/**
 * Pre-serialize a single OIS swap cash flow to a plain flow record. Mirrors
 * the legacy swap_leg_flow_builder shape verbatim, so the mapper can emit
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

OisSwapPerSwap priceTrade(const OisSwapTrade& trade,
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
    // raises if the requested index is not an OvernightIndex — preserves the
    // legacy parser's type check.
    auto overnightIndex = reg.rates.indices.getOvernightWithCurve(
        trade.overnight.indexId,
        QuantLib::Handle<QuantLib::YieldTermStructure>(fwdHandle.currentLink()));

    if (trade.fixed.notional != trade.overnight.notional) {
        // Preserves the legacy warning emitted by ois_swap_parser.
        std::cout << "Warning: Fixed and overnight notionals differ. Using fixed notional."
                  << std::endl;
    }

    QuantLib::Natural lookbackDays = trade.overnight.lookbackDays < 0
        ? QuantLib::Null<QuantLib::Natural>()
        : static_cast<QuantLib::Natural>(trade.overnight.lookbackDays);
    QuantLib::Natural lockoutDays =
        static_cast<QuantLib::Natural>(trade.overnight.lockoutDays);

    auto swap = std::make_shared<QuantLib::OvernightIndexedSwap>(
        trade.swapType,
        trade.fixed.notional,
        trade.fixed.schedule,
        trade.fixed.rate,
        trade.fixed.dayCounter,
        trade.overnight.schedule,
        overnightIndex,
        trade.overnight.spread,
        trade.overnight.paymentLag,
        trade.overnight.paymentConvention,
        trade.overnight.paymentCalendar,
        trade.overnight.telescopicValueDates,
        trade.overnight.averagingMethod,
        lookbackDays,
        lockoutDays,
        trade.overnight.applyObservationShift);
    swap->setPricingEngine(
        std::make_shared<QuantLib::DiscountingSwapEngine>(discHandle));

    OisSwapPerSwap out;
    out.npv = swap->NPV();
    out.fairRate = swap->fairRate();
    out.fairSpread = swap->fairSpread();
    out.fixedLegBps = swap->fixedLegBPS();
    out.overnightLegBps = swap->overnightLegBPS();
    out.fixedLegNpv = swap->fixedLegNPV();
    out.overnightLegNpv = swap->overnightLegNPV();
    out.includeFlows = includeFlows;

    if (includeFlows) {
        auto discountCurve = discHandle.currentLink();
        extractFlows(swap->fixedLeg(), *discountCurve, ctx.asOf, out.fixedFlows);
        extractFlows(swap->overnightLeg(), *discountCurve, ctx.asOf, out.overnightFlows);
    }
    return out;
}

} // namespace

OisSwapResult OisSwapPricer::price(const OisSwapInputs& inputs,
                                   const PricingRegistry& reg,
                                   const PricingContext& ctx) const {
    OisSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        result.swaps.push_back(priceTrade(trade, reg, ctx, inputs.includeFlows));
    }
    return result;
}

} // namespace quantra
