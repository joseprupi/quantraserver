#include "ois_swap_pricing_request.h"

#include <ql/cashflow.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>

#include "pricing_registry.h"
#include "ois_swap_parser.h"

using namespace QuantLib;
using namespace quantra;

namespace {
flatbuffers::Offset<SwapLegFlow> BuildLegFlow(
    const std::shared_ptr<CashFlow>& cf,
    const std::shared_ptr<YieldTermStructure>& discountCurve,
    flatbuffers::grpc::MessageBuilder& builder,
    Date asOf) {
    auto coupon = std::dynamic_pointer_cast<Coupon>(cf);
    if (!coupon || coupon->hasOccurred(asOf)) {
        return 0;
    }

    std::ostringstream osPayment, osStart, osEnd;
    osPayment << QuantLib::io::iso_date(coupon->date());
    osStart << QuantLib::io::iso_date(coupon->accrualStartDate());
    osEnd << QuantLib::io::iso_date(coupon->accrualEndDate());

    auto paymentDate = builder.CreateString(osPayment.str());
    auto accrualStart = builder.CreateString(osStart.str());
    auto accrualEnd = builder.CreateString(osEnd.str());

    double discount = discountCurve->discount(coupon->date());
    double amount = coupon->amount();

    SwapLegFlowBuilder fb(builder);
    fb.add_payment_date(paymentDate);
    fb.add_accrual_start_date(accrualStart);
    fb.add_accrual_end_date(accrualEnd);
    fb.add_amount(amount);
    fb.add_discount(discount);
    fb.add_present_value(amount * discount);
    fb.add_rate(coupon->rate());

    auto frc = std::dynamic_pointer_cast<FloatingRateCoupon>(coupon);
    if (frc) {
        std::ostringstream osFix;
        osFix << QuantLib::io::iso_date(frc->fixingDate());
        fb.add_fixing_date(builder.CreateString(osFix.str()));
        fb.add_index_fixing(frc->indexFixing());
        fb.add_spread(frc->spread());
    }
    return fb.Finish();
}
} // namespace

flatbuffers::Offset<PriceOisSwapResponse> OisSwapPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceOisSwapRequest* request) const {
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    OisSwapParser parser;
    Date asOf = Settings::instance().evaluationDate();
    bool includeFlows = request->include_flows();

    std::vector<flatbuffers::Offset<OisSwapResponse>> out;
    for (auto it = request->swaps()->begin(); it != request->swaps()->end(); ++it) {
        auto discIt = reg.curves.find(it->discounting_curve()->str());
        if (discIt == reg.curves.end()) {
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());
        }
        auto fwdIt = reg.curves.find(it->forwarding_curve()->str());
        if (fwdIt == reg.curves.end()) {
            QUANTRA_ERROR("Forwarding curve not found: " + it->forwarding_curve()->str());
        }

        parser.linkForwardingTermStructure(fwdIt->second->currentLink());
        auto swap = parser.parse(it->ois_swap(), reg.indices);
        swap->setPricingEngine(std::make_shared<DiscountingSwapEngine>(*discIt->second));

        std::vector<flatbuffers::Offset<SwapLegFlow>> fixedFlows;
        std::vector<flatbuffers::Offset<SwapLegFlow>> overnightFlows;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> fixedFlowsOff = 0;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> overnightFlowsOff = 0;
        if (includeFlows) {
            auto discountCurve = discIt->second->currentLink();
            for (const auto& cf : swap->fixedLeg()) {
                auto flow = BuildLegFlow(cf, discountCurve, *builder, asOf);
                if (flow.o) fixedFlows.push_back(flow);
            }
            for (const auto& cf : swap->overnightLeg()) {
                auto flow = BuildLegFlow(cf, discountCurve, *builder, asOf);
                if (flow.o) overnightFlows.push_back(flow);
            }
            fixedFlowsOff = builder->CreateVector(fixedFlows);
            overnightFlowsOff = builder->CreateVector(overnightFlows);
        }

        OisSwapResponseBuilder rb(*builder);
        rb.add_npv(swap->NPV());
        rb.add_fair_rate(swap->fairRate());
        rb.add_fair_spread(swap->fairSpread());
        rb.add_fixed_leg_bps(swap->fixedLegBPS());
        rb.add_overnight_leg_bps(swap->overnightLegBPS());
        rb.add_fixed_leg_npv(swap->fixedLegNPV());
        rb.add_overnight_leg_npv(swap->overnightLegNPV());
        if (includeFlows) {
            rb.add_fixed_leg_flows(fixedFlowsOff);
            rb.add_overnight_leg_flows(overnightFlowsOff);
        }
        out.push_back(rb.Finish());
    }

    auto swapsOff = builder->CreateVector(out);
    PriceOisSwapResponseBuilder resp(*builder);
    resp.add_swaps(swapsOff);
    return resp.Finish();
}
