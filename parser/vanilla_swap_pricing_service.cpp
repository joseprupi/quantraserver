#include "vanilla_swap_pricing_service.h"

#include <ql/cashflows/couponpricer.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>

#include "error.h"
#include "vanilla_swap_parser.h"

namespace quantra {

VanillaSwapPriceResult VanillaSwapPricingService::price(
    const quantra::PriceVanillaSwap* tradePricing,
    const PricingRegistry& reg,
    const QuantLib::Date& asOf) const {
    if (!tradePricing || !tradePricing->vanilla_swap()) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwap entry requires vanilla_swap");
    }

    auto discountIt = reg.rates.curves.find(tradePricing->discounting_curve()->str());
    if (discountIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + tradePricing->discounting_curve()->str());
    }
    auto forwardIt = reg.rates.curves.find(tradePricing->forwarding_curve()->str());
    if (forwardIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Forwarding curve not found: " + tradePricing->forwarding_curve()->str());
    }

    const auto* trade = tradePricing->vanilla_swap();
    if (!trade->fixed_leg()) {
        QUANTRA_INVALID_ARGUMENT("VanillaSwap fixed_leg not found");
    }

    const bool hasIborFloatLeg = (trade->floating_leg() != nullptr);
    const bool hasCmsLeg = (trade->cms_leg() != nullptr);
    if (hasIborFloatLeg == hasCmsLeg) {
        QUANTRA_INVALID_ARGUMENT("VanillaSwap must contain exactly one of floating_leg or cms_leg");
    }

    VanillaSwapParser swapParser;
    CmsLegParser cmsLegParser;
    VanillaSwapPriceResult out;

    if (hasIborFloatLeg) {
        swapParser.linkForwardingTermStructure(forwardIt->second->currentLink());
        auto swap = swapParser.parse(trade, reg.rates.indices);
        swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

        out.npv = swap->NPV();
        out.fairRate = swap->fairRate();
        out.fairSpread = swap->fairSpread();
        out.fixedLegNpv = swap->fixedLegNPV();
        out.floatingLegNpv = swap->floatingLegNPV();
        out.fixedLegBps = swap->fixedLegBPS();
        out.floatingLegBps = swap->floatingLegBPS();
        out.fixedLeg = swap->fixedLeg();
        out.floatingLeg = swap->floatingLeg();
        return out;
    }

    const auto* fixedLegFb = trade->fixed_leg();
    const auto* cmsLegFb = trade->cms_leg();
    if (!fixedLegFb->schedule()) {
        QUANTRA_INVALID_ARGUMENT("VanillaSwap fixed_leg schedule not found");
    }
    if (!cmsLegFb->swaption_vol_id()) {
        QUANTRA_INVALID_ARGUMENT("CMS leg requires swaption_vol_id");
    }

    auto volIt = reg.volatility.swaptionVols.find(cmsLegFb->swaption_vol_id()->str());
    if (volIt == reg.volatility.swaptionVols.end()) {
        QUANTRA_NOT_FOUND("CMS leg swaption vol not found: " + cmsLegFb->swaption_vol_id()->str());
    }
    if (volIt->second.referenceDate != asOf) {
        QUANTRA_INVALID_ARGUMENT(
            "Strict mode: pricing.as_of_date must equal CMS swaption vol referenceDate for vol '" +
            cmsLegFb->swaption_vol_id()->str() + "'");
    }

    ScheduleParser scheduleParser;
    auto fixedSchedule = scheduleParser.parse(fixedLegFb->schedule());
    out.fixedLeg = QuantLib::FixedRateLeg(*fixedSchedule)
                       .withNotionals(fixedLegFb->notional())
                       .withCouponRates(fixedLegFb->rate(), DayCounterToQL(fixedLegFb->day_counter()))
                       .withPaymentAdjustment(ConventionToQL(fixedLegFb->payment_convention()));

    out.floatingLeg = cmsLegParser.parse(
        cmsLegFb,
        reg.rates.indices,
        reg.rates.swapIndices,
        QuantLib::Handle<QuantLib::YieldTermStructure>(forwardIt->second->currentLink()),
        QuantLib::Handle<QuantLib::YieldTermStructure>(discountIt->second->currentLink()));

    auto cmsPricerBuild = cmsLegParser.makeCouponPricer(
        cmsLegFb,
        volIt->second,
        QuantLib::Handle<QuantLib::YieldTermStructure>(discountIt->second->currentLink()));
    QuantLib::setCouponPricer(out.floatingLeg, cmsPricerBuild.pricer);
    out.cmsUsed = cmsPricerBuild.used;

    const bool payerFixed = (trade->swap_type() == quantra::enums::SwapType_Payer);
    std::vector<QuantLib::Leg> legs{out.fixedLeg, out.floatingLeg};
    std::vector<bool> payer{payerFixed, !payerFixed};
    auto swap = std::make_shared<QuantLib::Swap>(legs, payer);
    swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    out.npv = swap->NPV();
    out.fixedLegNpv = swap->legNPV(0);
    out.floatingLegNpv = swap->legNPV(1);
    out.fixedLegBps = swap->legBPS(0);
    out.floatingLegBps = swap->legBPS(1);
    out.fairRate = 0.0;
    out.fairSpread = 0.0;
    return out;
}

} // namespace quantra
