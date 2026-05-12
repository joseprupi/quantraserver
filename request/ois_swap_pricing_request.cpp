#include "ois_swap_pricing_request.h"

#include <ql/cashflow.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>

#include "pricing_registry.h"
#include "ois_swap_parser.h"
#include "swap_leg_flow_builder.h"

using namespace QuantLib;
using namespace quantra;

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
        auto discIt = reg.rates.curves.find(it->discounting_curve()->str());
        if (discIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Discounting curve not found: " + it->discounting_curve()->str());
        }
        auto fwdIt = reg.rates.curves.find(it->forwarding_curve()->str());
        if (fwdIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Forwarding curve not found: " + it->forwarding_curve()->str());
        }

        parser.linkForwardingTermStructure(fwdIt->second->currentLink());
        auto swap = parser.parse(it->ois_swap(), reg.rates.indices);
        swap->setPricingEngine(std::make_shared<DiscountingSwapEngine>(*discIt->second));

        std::vector<flatbuffers::Offset<SwapLegFlow>> fixedFlows;
        std::vector<flatbuffers::Offset<SwapLegFlow>> overnightFlows;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> fixedFlowsOff = 0;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> overnightFlowsOff = 0;
        if (includeFlows) {
            auto discountCurve = discIt->second->currentLink();
            for (const auto& cf : swap->fixedLeg()) {
                auto flow = buildSwapLegFlow(cf, discountCurve, *builder, asOf);
                if (flow.o) fixedFlows.push_back(flow);
            }
            for (const auto& cf : swap->overnightLeg()) {
                auto flow = buildSwapLegFlow(cf, discountCurve, *builder, asOf);
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
