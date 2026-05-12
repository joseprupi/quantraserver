#include "year_on_year_inflation_swap_pricing_request.h"

#include <ql/settings.hpp>

#include "pricing_registry.h"
#include "swap_leg_flow_builder.h"
#include "year_on_year_inflation_swap_pricing_service.h"

using namespace QuantLib;
using namespace quantra;

namespace {

struct EvalDateGuard {
    QuantLib::Date saved;
    EvalDateGuard() : saved(QuantLib::Settings::instance().evaluationDate()) {}
    ~EvalDateGuard() { QuantLib::Settings::instance().evaluationDate() = saved; }
};

} // namespace

flatbuffers::Offset<PriceYearOnYearInflationSwapResponse> YearOnYearInflationSwapPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceYearOnYearInflationSwapRequest* request) const {
    EvalDateGuard evalDateGuard;
    if (!request || !request->pricing() || !request->pricing()->as_of_date()) {
        QUANTRA_INVALID_ARGUMENT("PriceYearOnYearInflationSwapRequest requires pricing.as_of_date");
    }
    Date asOf = DateToQL(request->pricing()->as_of_date()->str());
    Settings::instance().evaluationDate() = asOf;

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    YearOnYearInflationSwapPricingService pricingService;
    bool includeFlows = request->include_flows();

    std::vector<flatbuffers::Offset<YearOnYearInflationSwapResponse>> out;
    for (auto it = request->swaps()->begin(); it != request->swaps()->end(); ++it) {
        auto priced = pricingService.price(*it, reg);

        std::vector<flatbuffers::Offset<SwapLegFlow>> fixedFlows;
        std::vector<flatbuffers::Offset<SwapLegFlow>> yoyFlows;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> fixedFlowsOff = 0;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<SwapLegFlow>>> yoyFlowsOff = 0;

        if (includeFlows) {
            auto discountIt = reg.rates.curves.find(it->discounting_curve()->str());
            auto discountCurve = discountIt->second->currentLink();
            for (const auto& cf : priced.fixedLeg) {
                auto flow = buildSwapLegFlow(cf, discountCurve, *builder, asOf);
                if (flow.o) fixedFlows.push_back(flow);
            }
            for (const auto& cf : priced.yoyLeg) {
                auto flow = buildSwapLegFlow(cf, discountCurve, *builder, asOf);
                if (flow.o) yoyFlows.push_back(flow);
            }
            fixedFlowsOff = builder->CreateVector(fixedFlows);
            yoyFlowsOff = builder->CreateVector(yoyFlows);
        }

        YearOnYearInflationSwapResponseBuilder rb(*builder);
        rb.add_npv(priced.npv);
        rb.add_fair_rate(priced.fairRate);
        rb.add_fair_spread(priced.fairSpread);
        rb.add_fixed_leg_bps(priced.fixedLegBps);
        rb.add_yoy_leg_bps(priced.yoyLegBps);
        rb.add_fixed_leg_npv(priced.fixedLegNpv);
        rb.add_yoy_leg_npv(priced.yoyLegNpv);
        if (includeFlows) {
            rb.add_fixed_leg_flows(fixedFlowsOff);
            rb.add_yoy_leg_flows(yoyFlowsOff);
        }
        out.push_back(rb.Finish());
    }

    auto swapsOff = builder->CreateVector(out);
    PriceYearOnYearInflationSwapResponseBuilder resp(*builder);
    resp.add_swaps(swapsOff);
    return resp.Finish();
}
