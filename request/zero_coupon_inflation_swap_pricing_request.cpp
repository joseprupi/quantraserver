#include "zero_coupon_inflation_swap_pricing_request.h"

#include <ql/settings.hpp>

#include "pricing_registry.h"
#include "swap_leg_flow_builder.h"
#include "zero_coupon_inflation_swap_pricing_service.h"

using namespace QuantLib;
using namespace quantra;

namespace {

struct EvalDateGuard {
    QuantLib::Date saved;
    EvalDateGuard() : saved(QuantLib::Settings::instance().evaluationDate()) {}
    ~EvalDateGuard() { QuantLib::Settings::instance().evaluationDate() = saved; }
};

} // namespace

flatbuffers::Offset<PriceZeroCouponInflationSwapResponse> ZeroCouponInflationSwapPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceZeroCouponInflationSwapRequest* request) const {
    EvalDateGuard evalDateGuard;
    if (!request || !request->pricing() || !request->pricing()->as_of_date()) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponInflationSwapRequest requires pricing.as_of_date");
    }
    Date asOf = DateToQL(request->pricing()->as_of_date()->str());
    Settings::instance().evaluationDate() = asOf;

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    ZeroCouponInflationSwapPricingService pricingService;
    bool includeFlows = request->include_flows();

    std::vector<flatbuffers::Offset<ZeroCouponInflationSwapResponse>> out;
    for (auto it = request->swaps()->begin(); it != request->swaps()->end(); ++it) {
        auto priced = pricingService.price(*it, reg);

        std::vector<flatbuffers::Offset<SwapLegFlow>> fixedFlows;
        std::vector<flatbuffers::Offset<SwapLegFlow>> inflationFlows;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> fixedFlowsOff = 0;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> inflationFlowsOff = 0;

        if (includeFlows) {
            auto discountIt = reg.rates.curves.find(it->discounting_curve()->str());
            auto discountCurve = discountIt->second->currentLink();
            for (const auto& cf : priced.fixedLeg) {
                auto flow = buildSwapLegFlow(cf, discountCurve, *builder, asOf);
                if (flow.o) fixedFlows.push_back(flow);
            }
            for (const auto& cf : priced.inflationLeg) {
                auto flow = buildSwapLegFlow(cf, discountCurve, *builder, asOf);
                if (flow.o) inflationFlows.push_back(flow);
            }
            fixedFlowsOff = builder->CreateVector(fixedFlows);
            inflationFlowsOff = builder->CreateVector(inflationFlows);
        }

        ZeroCouponInflationSwapResponseBuilder rb(*builder);
        rb.add_npv(priced.npv);
        rb.add_fair_rate(priced.fairRate);
        rb.add_fixed_leg_bps(priced.fixedLegBps);
        rb.add_fixed_leg_npv(priced.fixedLegNpv);
        rb.add_inflation_leg_npv(priced.inflationLegNpv);
        if (includeFlows) {
            rb.add_fixed_leg_flows(fixedFlowsOff);
            rb.add_inflation_leg_flows(inflationFlowsOff);
        }
        out.push_back(rb.Finish());
    }

    auto swapsOff = builder->CreateVector(out);
    PriceZeroCouponInflationSwapResponseBuilder resp(*builder);
    resp.add_swaps(swapsOff);
    return resp.Finish();
}
