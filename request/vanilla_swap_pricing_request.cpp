#include "vanilla_swap_pricing_request.h"

#include "cms_leg_parser.h"
#include "pricing_registry.h"

#include <ql/cashflows/cmscoupon.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/cashflows/couponpricer.hpp>
#include <ql/instruments/swap.hpp>
#include <ql/settings.hpp>
#include <limits>

using namespace QuantLib;
using namespace quantra;

namespace {

struct EvalDateGuard {
    QuantLib::Date saved;
    EvalDateGuard() : saved(QuantLib::Settings::instance().evaluationDate()) {}
    ~EvalDateGuard() { QuantLib::Settings::instance().evaluationDate() = saved; }
};

} // namespace

flatbuffers::Offset<PriceVanillaSwapResponse> VanillaSwapPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceVanillaSwapRequest *request) const
{
    EvalDateGuard evalDateGuard;
    if (!request || !request->pricing() || !request->pricing()->as_of_date()) {
        QUANTRA_ERROR("PriceVanillaSwapRequest requires pricing.as_of_date");
    }
    Date as_of_date = DateToQL(request->pricing()->as_of_date()->str());
    Settings::instance().evaluationDate() = as_of_date;

    // Build registry (handles curves with dependency ordering via CurveBootstrapper)
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    VanillaSwapParser swap_parser;
    CmsLegParser cms_leg_parser;

    // Check if we should include flows
    bool include_flows = request->include_flows();

    // Process each swap
    auto swap_pricings = request->swaps();
    std::vector<flatbuffers::Offset<VanillaSwapResponse>> swaps_vector;

    for (auto it = swap_pricings->begin(); it != swap_pricings->end(); it++)
    {
        // Get discounting curve
        auto discounting_curve_it = reg.curves.find(it->discounting_curve()->str());
        if (discounting_curve_it == reg.curves.end())
        {
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());
        }

        // Get forwarding curve (may be same as discounting)
        auto forwarding_curve_it = reg.curves.find(it->forwarding_curve()->str());
        if (forwarding_curve_it == reg.curves.end())
        {
            QUANTRA_ERROR("Forwarding curve not found: " + it->forwarding_curve()->str());
        }

        const auto* trade = it->vanilla_swap();
        if (trade == nullptr || trade->fixed_leg() == nullptr) {
            QUANTRA_ERROR("VanillaSwap fixed_leg not found");
        }

        const bool hasIborFloatLeg = (trade->floating_leg() != nullptr);
        const bool hasCmsLeg = (trade->cms_leg() != nullptr);
        if (hasIborFloatLeg == hasCmsLeg) {
            QUANTRA_ERROR("VanillaSwap must contain exactly one of floating_leg or cms_leg");
        }

        double npv = 0.0;
        double fairRate = 0.0;
        double fairSpread = 0.0;
        double fixedLegNPV = 0.0;
        double floatingLegNPV = 0.0;
        double fixedLegBPS = 0.0;
        double floatingLegBPS = 0.0;
        auto usedCmsPricerType = quantra::enums::CmsPricerType_LinearTsr;
        auto usedCmsYieldCurveModel = quantra::enums::CmsYieldCurveModel_Standard;
        double usedCmsMeanReversion = -1.0;
        double usedCmsHaganLowerLimit = -1.0;
        double usedCmsHaganUpperLimit = -1.0;
        double usedCmsHaganPrecision = -1.0;
        double usedCmsHaganHardUpperLimit = -1.0;
        Leg fixedLeg;
        Leg floatingLeg;

        if (hasIborFloatLeg) {
            // Existing vanilla swap path
            swap_parser.linkForwardingTermStructure(forwarding_curve_it->second->currentLink());
            auto swap = swap_parser.parse(trade, reg.indices);
            auto engine = std::make_shared<DiscountingSwapEngine>(*discounting_curve_it->second);
            swap->setPricingEngine(engine);

            npv = swap->NPV();
            fairRate = swap->fairRate();
            fairSpread = swap->fairSpread();
            fixedLegNPV = swap->fixedLegNPV();
            floatingLegNPV = swap->floatingLegNPV();
            fixedLegBPS = swap->fixedLegBPS();
            floatingLegBPS = swap->floatingLegBPS();
            fixedLeg = swap->fixedLeg();
            floatingLeg = swap->floatingLeg();
        } else {
            // CMS path: fixed leg + CMS leg, priced with DiscountingSwapEngine.
            const auto* fixedLegFb = trade->fixed_leg();
            const auto* cmsLegFb = trade->cms_leg();
            if (!fixedLegFb->schedule()) {
                QUANTRA_ERROR("VanillaSwap fixed_leg schedule not found");
            }
            if (!cmsLegFb->swaption_vol_id()) {
                QUANTRA_ERROR("CMS leg requires swaption_vol_id");
            }

            auto volIt = reg.swaptionVols.find(cmsLegFb->swaption_vol_id()->str());
            if (volIt == reg.swaptionVols.end()) {
                QUANTRA_ERROR("CMS leg swaption vol not found: " + cmsLegFb->swaption_vol_id()->str());
            }
            if (volIt->second.referenceDate != as_of_date) {
                QUANTRA_ERROR(
                    "Strict mode: pricing.as_of_date must equal CMS swaption vol referenceDate for vol '" +
                    cmsLegFb->swaption_vol_id()->str() + "'");
            }

            ScheduleParser scheduleParser;
            auto fixedSchedule = scheduleParser.parse(fixedLegFb->schedule());
            fixedLeg = QuantLib::FixedRateLeg(*fixedSchedule)
                           .withNotionals(fixedLegFb->notional())
                           .withCouponRates(
                               fixedLegFb->rate(),
                               DayCounterToQL(fixedLegFb->day_counter()))
                           .withPaymentAdjustment(
                               ConventionToQL(fixedLegFb->payment_convention()));

            floatingLeg = cms_leg_parser.parse(
                cmsLegFb,
                reg.indices,
                reg.swapIndices,
                Handle<YieldTermStructure>(forwarding_curve_it->second->currentLink()),
                Handle<YieldTermStructure>(discounting_curve_it->second->currentLink()));
            auto cmsPricerBuild = cms_leg_parser.makeCouponPricer(
                cmsLegFb,
                volIt->second,
                Handle<YieldTermStructure>(discounting_curve_it->second->currentLink()));
            QuantLib::setCouponPricer(floatingLeg, cmsPricerBuild.pricer);
            usedCmsPricerType = cmsPricerBuild.used.pricerType;
            usedCmsYieldCurveModel = cmsPricerBuild.used.yieldCurveModel;
            usedCmsMeanReversion = cmsPricerBuild.used.meanReversion;
            usedCmsHaganLowerLimit = cmsPricerBuild.used.haganLowerLimit;
            usedCmsHaganUpperLimit = cmsPricerBuild.used.haganUpperLimit;
            usedCmsHaganPrecision = cmsPricerBuild.used.haganPrecision;
            usedCmsHaganHardUpperLimit = cmsPricerBuild.used.haganHardUpperLimit;

            bool payerFixed = (trade->swap_type() == quantra::enums::SwapType_Payer);
            std::vector<Leg> legs{fixedLeg, floatingLeg};
            std::vector<bool> payer{payerFixed, !payerFixed};
            auto swap = std::make_shared<QuantLib::Swap>(legs, payer);
            auto engine = std::make_shared<DiscountingSwapEngine>(*discounting_curve_it->second);
            swap->setPricingEngine(engine);

            npv = swap->NPV();
            fixedLegNPV = swap->legNPV(0);
            floatingLegNPV = swap->legNPV(1);
            fixedLegBPS = swap->legBPS(0);
            floatingLegBPS = swap->legBPS(1);
            // No closed-form fair rate/spread for generic fixed-vs-CMS swap in this endpoint.
            fairRate = 0.0;
            fairSpread = 0.0;
        }

        std::cout << "Swap NPV: " << npv << std::endl;

        // Build flows if requested
        std::vector<flatbuffers::Offset<SwapLegFlow>> fixed_leg_flows_vector;
        std::vector<flatbuffers::Offset<SwapLegFlow>> floating_leg_flows_vector;

        if (include_flows)
        {
            auto discountCurve = discounting_curve_it->second->currentLink();

            // Fixed leg flows
            const Leg& fixedLegRef = fixedLeg;
            for (const auto& cf : fixedLegRef)
            {
                auto coupon = std::dynamic_pointer_cast<FixedRateCoupon>(cf);
                if (coupon && !coupon->hasOccurred(as_of_date))
                {
                    std::ostringstream os_payment, os_start, os_end;
                    os_payment << QuantLib::io::iso_date(coupon->date());
                    os_start << QuantLib::io::iso_date(coupon->accrualStartDate());
                    os_end << QuantLib::io::iso_date(coupon->accrualEndDate());

                    auto payment_date = builder->CreateString(os_payment.str());
                    auto accrual_start = builder->CreateString(os_start.str());
                    auto accrual_end = builder->CreateString(os_end.str());

                    double discount = discountCurve->discount(coupon->date());
                    double pv = coupon->amount() * discount;

                    SwapLegFlowBuilder flow_builder(*builder);
                    flow_builder.add_payment_date(payment_date);
                    flow_builder.add_accrual_start_date(accrual_start);
                    flow_builder.add_accrual_end_date(accrual_end);
                    flow_builder.add_amount(coupon->amount());
                    flow_builder.add_accrual_year_fraction(coupon->accrualPeriod());
                    flow_builder.add_gearing(1.0);
                    flow_builder.add_discount(discount);
                    flow_builder.add_present_value(pv);
                    flow_builder.add_rate(coupon->rate());

                    fixed_leg_flows_vector.push_back(flow_builder.Finish());
                }
            }

            // Floating leg flows
            const Leg& floatingLegRef = floatingLeg;
            for (const auto& cf : floatingLegRef)
            {
                auto coupon = std::dynamic_pointer_cast<FloatingRateCoupon>(cf);
                if (coupon && !coupon->hasOccurred(as_of_date))
                {
                    std::ostringstream os_payment, os_start, os_end, os_fixing;
                    os_payment << QuantLib::io::iso_date(coupon->date());
                    os_start << QuantLib::io::iso_date(coupon->accrualStartDate());
                    os_end << QuantLib::io::iso_date(coupon->accrualEndDate());
                    os_fixing << QuantLib::io::iso_date(coupon->fixingDate());

                    auto payment_date = builder->CreateString(os_payment.str());
                    auto accrual_start = builder->CreateString(os_start.str());
                    auto accrual_end = builder->CreateString(os_end.str());
                    auto fixing_date = builder->CreateString(os_fixing.str());

                    double discount = discountCurve->discount(coupon->date());
                    double pv = coupon->amount() * discount;

                    SwapLegFlowBuilder flow_builder(*builder);
                    flow_builder.add_payment_date(payment_date);
                    flow_builder.add_accrual_start_date(accrual_start);
                    flow_builder.add_accrual_end_date(accrual_end);
                    flow_builder.add_amount(coupon->amount());
                    flow_builder.add_accrual_year_fraction(coupon->accrualPeriod());
                    flow_builder.add_gearing(coupon->gearing());
                    flow_builder.add_discount(discount);
                    flow_builder.add_present_value(pv);
                    flow_builder.add_fixing_date(fixing_date);
                    double fixing = std::numeric_limits<double>::quiet_NaN();
                    bool hasCmsSwapRate = false;
                    double cmsSwapRate = 0.0;
                    try {
                        fixing = coupon->indexFixing();
                        if (std::dynamic_pointer_cast<CmsCoupon>(coupon)) {
                            // For CMS coupons, indexFixing corresponds to the observed swap rate fixing.
                            hasCmsSwapRate = true;
                            cmsSwapRate = fixing;
                        }
                    } catch (...) {
                        // Best-effort flows view: missing historical fixings should not fail pricing response.
                    }
                    flow_builder.add_index_fixing(fixing);
                    flow_builder.add_has_cms_swap_rate(hasCmsSwapRate);
                    if (hasCmsSwapRate) {
                        flow_builder.add_cms_swap_rate(cmsSwapRate);
                    }
                    flow_builder.add_spread(coupon->spread());
                    flow_builder.add_rate(coupon->rate());

                    floating_leg_flows_vector.push_back(flow_builder.Finish());
                }
            }
        }

        auto fixed_leg_flows = builder->CreateVector(fixed_leg_flows_vector);
        auto floating_leg_flows = builder->CreateVector(floating_leg_flows_vector);

        // Build swap response
        VanillaSwapResponseBuilder swap_response_builder(*builder);
        swap_response_builder.add_npv(npv);
        swap_response_builder.add_fair_rate(fairRate);
        swap_response_builder.add_fair_spread(fairSpread);
        swap_response_builder.add_fixed_leg_bps(fixedLegBPS);
        swap_response_builder.add_floating_leg_bps(floatingLegBPS);
        swap_response_builder.add_fixed_leg_npv(fixedLegNPV);
        swap_response_builder.add_floating_leg_npv(floatingLegNPV);
        swap_response_builder.add_used_cms_pricer_type(usedCmsPricerType);
        swap_response_builder.add_used_cms_yield_curve_model(usedCmsYieldCurveModel);
        swap_response_builder.add_used_cms_mean_reversion(usedCmsMeanReversion);
        swap_response_builder.add_used_cms_hagan_lower_limit(usedCmsHaganLowerLimit);
        swap_response_builder.add_used_cms_hagan_upper_limit(usedCmsHaganUpperLimit);
        swap_response_builder.add_used_cms_hagan_precision(usedCmsHaganPrecision);
        swap_response_builder.add_used_cms_hagan_hard_upper_limit(usedCmsHaganHardUpperLimit);

        if (include_flows)
        {
            swap_response_builder.add_fixed_leg_flows(fixed_leg_flows);
            swap_response_builder.add_floating_leg_flows(floating_leg_flows);
        }

        swaps_vector.push_back(swap_response_builder.Finish());
    }

    // Build final response
    auto swaps = builder->CreateVector(swaps_vector);
    PriceVanillaSwapResponseBuilder response_builder(*builder);
    response_builder.add_swaps(swaps);

    return response_builder.Finish();
}
