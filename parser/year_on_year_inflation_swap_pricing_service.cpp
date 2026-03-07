#include "year_on_year_inflation_swap_pricing_service.h"

#include <ql/cashflows/inflationcouponpricer.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>

#include "error.h"
#include "year_on_year_inflation_swap_parser.h"

namespace quantra {

YearOnYearInflationSwapPriceResult YearOnYearInflationSwapPricingService::price(
    const quantra::PriceYearOnYearInflationSwap* tradePricing,
    const PricingRegistry& reg) const {
    if (!tradePricing || !tradePricing->year_on_year_inflation_swap()) {
        QUANTRA_ERROR("PriceYearOnYearInflationSwap entry requires year_on_year_inflation_swap");
    }
    if (!tradePricing->discounting_curve()) {
        QUANTRA_ERROR("PriceYearOnYearInflationSwap entry requires discounting_curve");
    }
    if (!tradePricing->inflation_curve()) {
        QUANTRA_ERROR("PriceYearOnYearInflationSwap entry requires inflation_curve");
    }

    const auto* trade = tradePricing->year_on_year_inflation_swap();

    auto discountIt = reg.rates.curves.find(tradePricing->discounting_curve()->str());
    if (discountIt == reg.rates.curves.end()) {
        QUANTRA_ERROR("Discounting curve not found: " + tradePricing->discounting_curve()->str());
    }

    const std::string inflationCurveId = tradePricing->inflation_curve()->str();
    auto curveIt = reg.inflation.yoyInflationCurves.find(inflationCurveId);
    if (curveIt == reg.inflation.yoyInflationCurves.end() || !curveIt->second || curveIt->second->empty()) {
        QUANTRA_ERROR("YoY inflation curve not found: " + inflationCurveId);
    }

    auto metaIt = reg.inflation.curveMetadata.find(inflationCurveId);
    if (metaIt == reg.inflation.curveMetadata.end()) {
        QUANTRA_ERROR("Inflation curve metadata not found: " + inflationCurveId);
    }
    if (metaIt->second.kind != enums::InflationCurveKind_YoYInflation) {
        QUANTRA_ERROR("Inflation curve is not YoY inflation: " + inflationCurveId);
    }

    const std::string indexId = trade->inflation_index_id()->str();
    if (metaIt->second.indexId != indexId) {
        QUANTRA_ERROR(
            "YearOnYearInflationSwap inflation_index_id does not match inflation_curve '" +
            inflationCurveId + "'");
    }

    auto indexIt = reg.inflation.inflationIndices.find(indexId);
    if (indexIt == reg.inflation.inflationIndices.end()) {
        QUANTRA_ERROR("Inflation index not found: " + indexId);
    }
    auto inflationIndex = std::dynamic_pointer_cast<QuantLib::YoYInflationIndex>(indexIt->second);
    if (!inflationIndex) {
        QUANTRA_ERROR("Inflation index is not YoY inflation: " + indexId);
    }

    YearOnYearInflationSwapParser parser;
    auto swap = parser.parse(trade, inflationIndex);
    QuantLib::setCouponPricer(
        swap->yoyLeg(),
        QuantLib::ext::make_shared<QuantLib::BlackYoYInflationCouponPricer>(
            QuantLib::Handle<QuantLib::YieldTermStructure>(discountIt->second->currentLink())));
    swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    YearOnYearInflationSwapPriceResult out;
    out.npv = swap->NPV();
    out.fairRate = swap->fairRate();
    out.fairSpread = swap->fairSpread();
    out.fixedLegBps = swap->legBPS(0);
    out.yoyLegBps = swap->legBPS(1);
    out.fixedLegNpv = swap->fixedLegNPV();
    out.yoyLegNpv = swap->yoyLegNPV();
    out.fixedLeg = swap->fixedLeg();
    out.yoyLeg = swap->yoyLeg();
    return out;
}

} // namespace quantra
