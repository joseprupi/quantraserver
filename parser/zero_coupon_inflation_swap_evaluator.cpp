#include "zero_coupon_inflation_swap_evaluator.h"

#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/handle.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>

#include "common.h"   // DateToIso
#include "error.h"

namespace quantra {

namespace {

/**
 * Pre-serialize a swap-leg cashflow to a plain flow record. Mirrors
 * parser/swap_leg_flow_builder.cpp verbatim — including its rule that
 * accrual/fixing fields are emitted only when the cashflow is a Coupon /
 * FloatingRateCoupon respectively, and that occurred cashflows are skipped.
 * Returns false when the cashflow has occurred and the mapper must drop it.
 */
bool extractFlow(const std::shared_ptr<QuantLib::CashFlow>& cf,
                 const QuantLib::YieldTermStructure& discountCurve,
                 const QuantLib::Date& asOf,
                 ZeroCouponInflationSwapFlowPlain& out) {
    if (!cf || cf->hasOccurred(asOf)) {
        return false;
    }
    out.paymentDate = DateToIso(cf->date());
    out.amount = cf->amount();
    out.discount = discountCurve.discount(cf->date());
    out.presentValue = out.amount * out.discount;

    auto coupon = std::dynamic_pointer_cast<QuantLib::Coupon>(cf);
    if (coupon) {
        out.isCoupon = true;
        out.accrualStartDate = DateToIso(coupon->accrualStartDate());
        out.accrualEndDate = DateToIso(coupon->accrualEndDate());
        out.accrualYearFraction = coupon->accrualPeriod();
        out.rate = coupon->rate();
    }

    auto floating = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(coupon);
    if (floating) {
        out.isFloating = true;
        out.fixingDate = DateToIso(floating->fixingDate());
        out.indexFixing = floating->indexFixing();
        out.spread = floating->spread();
    }
    return true;
}

ZeroCouponInflationSwapPerSwap priceTrade(const ZeroCouponInflationSwapTrade& trade,
                                          const PricingRegistry& reg,
                                          const PricingContext& ctx,
                                          bool includeFlows) {
    auto discountIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discountIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }

    auto curveIt = reg.inflation.zeroInflationCurves.find(trade.inflationCurveId);
    if (curveIt == reg.inflation.zeroInflationCurves.end() ||
        !curveIt->second || curveIt->second->empty()) {
        QUANTRA_NOT_FOUND("Zero inflation curve not found: " + trade.inflationCurveId);
    }

    auto metaIt = reg.inflation.curveMetadata.find(trade.inflationCurveId);
    if (metaIt == reg.inflation.curveMetadata.end()) {
        QUANTRA_ERROR("Inflation curve metadata not found: " + trade.inflationCurveId);
    }
    if (metaIt->second.kind != enums::InflationCurveKind_ZeroInflation) {
        QUANTRA_INVALID_ARGUMENT("Inflation curve is not zero inflation: " + trade.inflationCurveId);
    }
    if (metaIt->second.indexId != trade.inflationIndexId) {
        QUANTRA_INVALID_ARGUMENT(
            "ZeroCouponInflationSwap inflation_index_id does not match inflation_curve '" +
            trade.inflationCurveId + "'");
    }

    auto indexIt = reg.inflation.inflationIndices.find(trade.inflationIndexId);
    if (indexIt == reg.inflation.inflationIndices.end()) {
        QUANTRA_NOT_FOUND("Inflation index not found: " + trade.inflationIndexId);
    }
    auto inflationIndex =
        std::dynamic_pointer_cast<QuantLib::ZeroInflationIndex>(indexIt->second);
    if (!inflationIndex) {
        QUANTRA_INVALID_ARGUMENT("Inflation index is not zero inflation: " + trade.inflationIndexId);
    }

    auto swap = std::make_shared<QuantLib::ZeroCouponInflationSwap>(
        trade.swapType,
        trade.notional,
        trade.startDate,
        trade.maturityDate,
        trade.fixedCalendar,
        trade.fixedConvention,
        trade.dayCounter,
        trade.fixedRate,
        inflationIndex,
        trade.observationLag,
        trade.observationInterpolation,
        trade.adjustObservationDates,
        trade.inflationCalendar,
        trade.inflationConvention);
    swap->setPricingEngine(
        std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    ZeroCouponInflationSwapPerSwap out;
    out.npv = swap->NPV();
    out.fairRate = swap->fairRate();
    out.fixedLegBps = swap->fixedLegBPS();
    out.fixedLegNpv = swap->fixedLegNPV();
    out.inflationLegNpv = swap->inflationLegNPV();
    out.includeFlows = includeFlows;

    if (includeFlows) {
        auto discountCurve = discountIt->second->currentLink();
        for (const auto& cf : swap->fixedLeg()) {
            ZeroCouponInflationSwapFlowPlain flow;
            if (extractFlow(cf, *discountCurve, ctx.asOf, flow)) {
                out.fixedFlows.push_back(std::move(flow));
            }
        }
        for (const auto& cf : swap->inflationLeg()) {
            ZeroCouponInflationSwapFlowPlain flow;
            if (extractFlow(cf, *discountCurve, ctx.asOf, flow)) {
                out.inflationFlows.push_back(std::move(flow));
            }
        }
    }
    return out;
}

} // namespace

ZeroCouponInflationSwapResult ZeroCouponInflationSwapEvaluator::evaluate(
    const ZeroCouponInflationSwapInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {

    ZeroCouponInflationSwapResult result;
    result.swaps.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        result.swaps.push_back(priceTrade(trade, reg, ctx, inputs.includeFlows));
    }
    return result;
}

} // namespace quantra
