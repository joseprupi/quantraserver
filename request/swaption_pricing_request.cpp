#include "swaption_pricing_request.h"

#include "pricing_registry.h"
#include "vol_surface_parsers.h"
#include "engine_factory.h"
#include "swaption_parser.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceSwaptionResponse> SwaptionPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceSwaptionRequest *request) const
{
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    SwaptionParser swaption_parser;
    EngineFactory engineFactory;

    auto swaption_pricings = request->swaptions();
    std::vector<flatbuffers::Offset<SwaptionResponse>> swaptions_vector;

    for (auto it = swaption_pricings->begin(); it != swaption_pricings->end(); it++)
    {
        auto dIt = reg.curves.find(it->discounting_curve()->str());
        if (dIt == reg.curves.end())
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());

        auto fIt = reg.curves.find(it->forwarding_curve()->str());
        if (fIt == reg.curves.end())
            QUANTRA_ERROR("Forwarding curve not found: " + it->forwarding_curve()->str());

        auto vIt = reg.swaptionVols.find(it->volatility()->str());
        if (vIt == reg.swaptionVols.end())
            QUANTRA_ERROR("Swaption vol not found: " + it->volatility()->str());

        auto mIt = reg.models.find(it->model()->str());
        if (mIt == reg.models.end())
            QUANTRA_ERROR("Model not found: " + it->model()->str());

        swaption_parser.linkForwardingTermStructure(fIt->second->currentLink());
        auto swaption = swaption_parser.parse(it->swaption(), reg.indices);

        Handle<YieldTermStructure> discountCurve(dIt->second->currentLink());
        auto engine = engineFactory.makeSwaptionEngine(mIt->second, discountCurve, vIt->second);
        swaption->setPricingEngine(engine);

        double npv = swaption->NPV();

        std::cout << "Swaption NPV: " << npv << std::endl;

        SwaptionResponseBuilder response_builder(*builder);
        response_builder.add_npv(npv);
        response_builder.add_implied_volatility(vIt->second.constantVol);
        response_builder.add_atm_forward(0.0);
        response_builder.add_annuity(0.0);
        response_builder.add_delta(0.0);
        response_builder.add_vega(0.0);

        swaptions_vector.push_back(response_builder.Finish());
    }

    auto swaptions = builder->CreateVector(swaptions_vector);
    PriceSwaptionResponseBuilder response_builder(*builder);
    response_builder.add_swaptions(swaptions);

    return response_builder.Finish();
}
