#include "fra_pricing_request.h"

#include "pricing_registry.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceFRAResponse> FRAPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceFRARequest *request) const
{
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    FRAParser fra_parser;

    auto fra_pricings = request->fras();
    std::vector<flatbuffers::Offset<FRAResponse>> fras_vector;

    for (auto it = fra_pricings->begin(); it != fra_pricings->end(); it++)
    {
        auto discounting_curve_it = reg.curves.find(it->discounting_curve()->str());
        if (discounting_curve_it == reg.curves.end())
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());

        auto forwarding_curve_it = reg.curves.find(it->forwarding_curve()->str());
        if (forwarding_curve_it == reg.curves.end())
            QUANTRA_ERROR("Forwarding curve not found: " + it->forwarding_curve()->str());

        fra_parser.linkForwardingTermStructure(forwarding_curve_it->second->currentLink());
        fra_parser.linkDiscountingTermStructure(discounting_curve_it->second->currentLink());
        auto fra = fra_parser.parse(it->fra(), reg.indices);

        double npv = fra->NPV();
        double forwardRate = fra->forwardRate();
        double spotValue = npv;

        std::cout << "FRA NPV: " << npv << ", Forward Rate: " << forwardRate * 100 << "%" << std::endl;

        auto settlement_date_str = builder->CreateString(it->fra()->start_date()->str());

        FRAResponseBuilder fra_response_builder(*builder);
        fra_response_builder.add_npv(npv);
        fra_response_builder.add_forward_rate(forwardRate);
        fra_response_builder.add_spot_value(spotValue);
        fra_response_builder.add_settlement_date(settlement_date_str);

        fras_vector.push_back(fra_response_builder.Finish());
    }

    auto fras = builder->CreateVector(fras_vector);
    PriceFRAResponseBuilder response_builder(*builder);
    response_builder.add_fras(fras);

    return response_builder.Finish();
}
