#include "zero_coupon_inflation_swap_pricing_service.h"

#include <ql/pricingengines/swap/discountingswapengine.hpp>

#include "error.h"
#include "zero_coupon_inflation_swap_parser.h"

namespace quantra {

ZeroCouponInflationSwapPriceResult ZeroCouponInflationSwapPricingService::price(
    const quantra::PriceZeroCouponInflationSwap* tradePricing,
    const PricingRegistry& reg) const {
    if (!tradePricing || !tradePricing->zero_coupon_inflation_swap()) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponInflationSwap entry requires zero_coupon_inflation_swap");
    }
    if (!tradePricing->discounting_curve()) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponInflationSwap entry requires discounting_curve");
    }
    if (!tradePricing->inflation_curve()) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponInflationSwap entry requires inflation_curve");
    }

    const auto* trade = tradePricing->zero_coupon_inflation_swap();

    auto discountIt = reg.rates.curves.find(tradePricing->discounting_curve()->str());
    if (discountIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + tradePricing->discounting_curve()->str());
    }

    const std::string inflationCurveId = tradePricing->inflation_curve()->str();
    auto curveIt = reg.inflation.zeroInflationCurves.find(inflationCurveId);
    if (curveIt == reg.inflation.zeroInflationCurves.end() || !curveIt->second || curveIt->second->empty()) {
        QUANTRA_NOT_FOUND("Zero inflation curve not found: " + inflationCurveId);
    }

    auto metaIt = reg.inflation.curveMetadata.find(inflationCurveId);
    if (metaIt == reg.inflation.curveMetadata.end()) {
        QUANTRA_NOT_FOUND("Inflation curve metadata not found: " + inflationCurveId);
    }
    if (metaIt->second.kind != enums::InflationCurveKind_ZeroInflation) {
        QUANTRA_INVALID_ARGUMENT("Inflation curve is not zero inflation: " + inflationCurveId);
    }

    const std::string indexId = trade->inflation_index_id()->str();
    if (metaIt->second.indexId != indexId) {
        QUANTRA_INVALID_ARGUMENT(
            "ZeroCouponInflationSwap inflation_index_id does not match inflation_curve '" +
            inflationCurveId + "'");
    }

    auto indexIt = reg.inflation.inflationIndices.find(indexId);
    if (indexIt == reg.inflation.inflationIndices.end()) {
        QUANTRA_NOT_FOUND("Inflation index not found: " + indexId);
    }
    auto inflationIndex = std::dynamic_pointer_cast<QuantLib::ZeroInflationIndex>(indexIt->second);
    if (!inflationIndex) {
        QUANTRA_INVALID_ARGUMENT("Inflation index is not zero inflation: " + indexId);
    }

    ZeroCouponInflationSwapParser parser;
    auto swap = parser.parse(trade, inflationIndex);
    swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    ZeroCouponInflationSwapPriceResult out;
    out.npv = swap->NPV();
    out.fairRate = swap->fairRate();
    out.fixedLegBps = swap->fixedLegBPS();
    out.fixedLegNpv = swap->fixedLegNPV();
    out.inflationLegNpv = swap->inflationLegNPV();
    out.fixedLeg = swap->fixedLeg();
    out.inflationLeg = swap->inflationLeg();
    return out;
}

} // namespace quantra
