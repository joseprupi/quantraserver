#include "vanilla_swap_pricing_request.h"

#include "pricing_registry.h"
#include "vanilla_swap_flow_builder.h"
#include "vanilla_swap_pricing_service.h"

#include <ql/settings.hpp>

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

    VanillaSwapPricingService pricingService;

    // Check if we should include flows
    bool include_flows = request->include_flows();

    // Process each swap
    auto swap_pricings = request->swaps();
    std::vector<flatbuffers::Offset<VanillaSwapResponse>> swaps_vector;

    for (auto it = swap_pricings->begin(); it != swap_pricings->end(); it++)
    {
        VanillaSwapPriceResult priced = pricingService.price(*it, reg, as_of_date);

        std::cout << "Swap NPV: " << priced.npv << std::endl;

        // Build flows if requested
        std::vector<flatbuffers::Offset<SwapLegFlow>> fixed_leg_flows_vector;
        std::vector<flatbuffers::Offset<SwapLegFlow>> floating_leg_flows_vector;

        if (include_flows)
        {
            auto discountIt = reg.rates.curves.find(it->discounting_curve()->str());
            auto builtFlows = buildVanillaSwapFlows(
                priced.fixedLeg,
                priced.floatingLeg,
                discountIt->second->currentLink(),
                builder,
                as_of_date);
            fixed_leg_flows_vector = std::move(builtFlows.fixedFlows);
            floating_leg_flows_vector = std::move(builtFlows.floatingFlows);
        }

        auto fixed_leg_flows = builder->CreateVector(fixed_leg_flows_vector);
        auto floating_leg_flows = builder->CreateVector(floating_leg_flows_vector);

        // Build swap response
        VanillaSwapResponseBuilder swap_response_builder(*builder);
        swap_response_builder.add_npv(priced.npv);
        swap_response_builder.add_fair_rate(priced.fairRate);
        swap_response_builder.add_fair_spread(priced.fairSpread);
        swap_response_builder.add_fixed_leg_bps(priced.fixedLegBps);
        swap_response_builder.add_floating_leg_bps(priced.floatingLegBps);
        swap_response_builder.add_fixed_leg_npv(priced.fixedLegNpv);
        swap_response_builder.add_floating_leg_npv(priced.floatingLegNpv);
        const bool hasCmsLeg = it->vanilla_swap() && it->vanilla_swap()->cms_leg();
        if (hasCmsLeg) {
            swap_response_builder.add_used_cms_pricer_type(priced.cmsUsed.pricerType);
            swap_response_builder.add_used_cms_yield_curve_model(priced.cmsUsed.yieldCurveModel);
            swap_response_builder.add_used_cms_mean_reversion(priced.cmsUsed.meanReversion);
            swap_response_builder.add_used_cms_hagan_lower_limit(priced.cmsUsed.haganLowerLimit);
            swap_response_builder.add_used_cms_hagan_upper_limit(priced.cmsUsed.haganUpperLimit);
            swap_response_builder.add_used_cms_hagan_precision(priced.cmsUsed.haganPrecision);
            swap_response_builder.add_used_cms_hagan_hard_upper_limit(priced.cmsUsed.haganHardUpperLimit);
        }

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
