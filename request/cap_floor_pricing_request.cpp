#include "cap_floor_pricing_request.h"

#include "cap_floor_flow_builder.h"
#include "pricing_registry.h"
#include "vol_surface_parsers.h"
#include "engine_factory.h"
#include "cap_floor_parser.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceCapFloorResponse> CapFloorPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceCapFloorRequest *request) const
{
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    CapFloorParser cap_floor_parser;
    EngineFactory engineFactory;

    Date as_of_date = Settings::instance().evaluationDate();

    auto cap_floor_pricings = request->cap_floors();
    std::vector<flatbuffers::Offset<CapFloorResponse>> cap_floors_vector;

    for (auto it = cap_floor_pricings->begin(); it != cap_floor_pricings->end(); it++)
    {
        auto dIt = reg.rates.curves.find(it->discounting_curve()->str());
        if (dIt == reg.rates.curves.end())
            QUANTRA_NOT_FOUND("Discounting curve not found: " + it->discounting_curve()->str());

        auto fIt = reg.rates.curves.find(it->forwarding_curve()->str());
        if (fIt == reg.rates.curves.end())
            QUANTRA_NOT_FOUND("Forwarding curve not found: " + it->forwarding_curve()->str());

        auto vIt = reg.volatility.optionletVols.find(it->volatility()->str());
        if (vIt == reg.volatility.optionletVols.end())
            QUANTRA_NOT_FOUND("Optionlet vol not found: " + it->volatility()->str());

        auto mIt = reg.volatility.models.find(it->model()->str());
        if (mIt == reg.volatility.models.end())
            QUANTRA_NOT_FOUND("Model not found: " + it->model()->str());

        cap_floor_parser.linkForwardingTermStructure(fIt->second->currentLink());
        auto capFloor = cap_floor_parser.parse(it->cap_floor(), reg.rates.indices);

        Handle<YieldTermStructure> discountCurve(dIt->second->currentLink());
        auto engine = engineFactory.makeCapFloorEngine(mIt->second, discountCurve, vIt->second);
        capFloor->setPricingEngine(engine);

        double npv = capFloor->NPV();
        double atmRate = capFloor->atmRate(*dIt->second->currentLink());

        std::cout << "CapFloor NPV: " << npv << ", ATM Rate: " << atmRate * 100 << "%" << std::endl;

        std::vector<flatbuffers::Offset<CapFloorLet>> capfloorlets_vector;
        if (it->include_details()) {
            capfloorlets_vector = buildCapFloorDetails(
                capFloor->floatingLeg(),
                dIt->second->currentLink(),
                builder,
                as_of_date);
        }

        auto capfloorlets = builder->CreateVector(capfloorlets_vector);

        CapFloorResponseBuilder response(*builder);
        response.add_npv(npv);
        response.add_atm_rate(atmRate);
        response.add_cap_floor_lets(capfloorlets);

        cap_floors_vector.push_back(response.Finish());
    }

    auto cap_floors = builder->CreateVector(cap_floors_vector);
    PriceCapFloorResponseBuilder response_builder(*builder);
    response_builder.add_cap_floors(cap_floors);

    return response_builder.Finish();
}
