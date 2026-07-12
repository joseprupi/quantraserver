#include "zero_coupon_swap_evaluator.h"

#include <memory>

#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "date_convert.h"
#include "error.h"

namespace quantra {

namespace {

ZeroCouponSwapPerSwap priceTrade(const ZeroCouponSwapTrade& trade,
                                 const PricingRegistry& reg,
                                 const PricingContext& ctx) {
    (void)ctx;
    auto discIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }
    auto fwdIt = reg.rates.curves.find(trade.forwardingCurveId);
    if (fwdIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Forwarding curve not found: " + trade.forwardingCurveId);
    }
    if (!reg.rates.indices.has(trade.indexId)) {
        QUANTRA_NOT_FOUND("Index not found: " + trade.indexId);
    }

    const auto& discHandle = *discIt->second;
    const auto& fwdHandle = *fwdIt->second;

    // IndexRegistry::getIborWithCurve downcasts the registry entry to IborIndex
    // and raises if the requested index is not an IborIndex.
    auto iborIndex = reg.rates.indices.getIborWithCurve(
        trade.indexId,
        QuantLib::Handle<QuantLib::YieldTermStructure>(fwdHandle.currentLink()));

    std::shared_ptr<QuantLib::ZeroCouponSwap> swap;
    if (trade.hasFixedRate) {
        swap = std::make_shared<QuantLib::ZeroCouponSwap>(
            trade.swapType,
            trade.baseNominal,
            trade.startDate,
            trade.maturityDate,
            trade.fixedRate,
            trade.fixedRateDc,
            iborIndex,
            trade.paymentCalendar,
            trade.paymentConvention,
            static_cast<QuantLib::Natural>(trade.paymentDelay));
    } else {
        swap = std::make_shared<QuantLib::ZeroCouponSwap>(
            trade.swapType,
            trade.baseNominal,
            trade.startDate,
            trade.maturityDate,
            trade.fixedPayment,
            iborIndex,
            trade.paymentCalendar,
            trade.paymentConvention,
            static_cast<QuantLib::Natural>(trade.paymentDelay));
    }

    swap->setPricingEngine(
        std::make_shared<QuantLib::DiscountingSwapEngine>(
            QuantLib::Handle<QuantLib::YieldTermStructure>(discHandle.currentLink())));

    ZeroCouponSwapPerSwap out;
    out.npv = swap->NPV();
    out.fixedLegNpv = swap->fixedLegNPV();
    out.floatingLegNpv = swap->floatingLegNPV();
    out.fairFixedPayment = swap->fairFixedPayment();
    out.fixedPayment = swap->fixedPayment();
    out.startDate = DateToIso(swap->startDate());
    if (trade.hasFixedRate) {
        out.hasFairFixedRate = true;
        out.fairFixedRate = swap->fairFixedRate(trade.fixedRateDc);
    }
    return out;
}

} // namespace

ZeroCouponSwapResult ZeroCouponSwapEvaluator::evaluate(
    const ZeroCouponSwapInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    ZeroCouponSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        ctx.budget.check();  // honor the per-request deadline before each trade
        result.swaps.push_back(priceTrade(trade, reg, ctx));
    }
    return result;
}

} // namespace quantra
