#include "vanilla_swap_pricing_request.h"

#include "pricing_registry.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceVanillaSwapResponse> VanillaSwapPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceVanillaSwapRequest *request) const
{
    // Build registry (handles curves with dependency ordering via CurveBootstrapper)
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    VanillaSwapParser swap_parser;
    Date as_of_date = Settings::instance().evaluationDate();

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

        // Link forwarding curve to parser and parse swap
        swap_parser.linkForwardingTermStructure(forwarding_curve_it->second->currentLink());
        auto swap = swap_parser.parse(it->vanilla_swap(), reg.indices);

        // Set pricing engine
        auto engine = std::make_shared<DiscountingSwapEngine>(*discounting_curve_it->second);
        swap->setPricingEngine(engine);

        // Calculate results
        double npv = swap->NPV();
        double fairRate = swap->fairRate();
        double fairSpread = swap->fairSpread();
        double fixedLegNPV = swap->fixedLegNPV();
        double floatingLegNPV = swap->floatingLegNPV();
        double fixedLegBPS = swap->fixedLegBPS();
        double floatingLegBPS = swap->floatingLegBPS();

        std::cout << "Swap NPV: " << npv << std::endl;

        // Build flows if requested
        std::vector<flatbuffers::Offset<SwapLegFlow>> fixed_leg_flows_vector;
        std::vector<flatbuffers::Offset<SwapLegFlow>> floating_leg_flows_vector;

        if (include_flows)
        {
            auto discountCurve = discounting_curve_it->second->currentLink();

            // Fixed leg flows
            const Leg& fixedLeg = swap->fixedLeg();
            for (const auto& cf : fixedLeg)
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
                    flow_builder.add_discount(discount);
                    flow_builder.add_present_value(pv);
                    flow_builder.add_rate(coupon->rate());

                    fixed_leg_flows_vector.push_back(flow_builder.Finish());
                }
            }

            // Floating leg flows
            const Leg& floatingLeg = swap->floatingLeg();
            for (const auto& cf : floatingLeg)
            {
                auto coupon = std::dynamic_pointer_cast<IborCoupon>(cf);
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
                    flow_builder.add_discount(discount);
                    flow_builder.add_present_value(pv);
                    flow_builder.add_fixing_date(fixing_date);
                    flow_builder.add_index_fixing(coupon->indexFixing());
                    flow_builder.add_spread(coupon->spread());

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
