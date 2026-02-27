#include "cds_pricing_request.h"

#include "cds_pricing_service.h"
#include "pricing_registry.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceCDSResponse> CDSPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceCDSRequest *request) const
{
    // Build registry (handles curves with dependency ordering via CurveBootstrapper)
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    CdsPricingService pricingService;

    // Process each CDS
    auto cds_pricings = request->cds_list();
    std::vector<flatbuffers::Offset<CDSValues>> cds_vector;

    for (auto it = cds_pricings->begin(); it != cds_pricings->end(); it++)
    {
        try
        {
            CdsPriceResult priced = pricingService.price(*it, reg);
            std::cout << "CDS NPV: " << priced.npv
                      << ", Fair Spread: " << priced.fairSpread * 10000 << " bps" << std::endl;

            // Build response
            CDSValuesBuilder response_builder(*builder);
            response_builder.add_npv(priced.npv);
            response_builder.add_fair_spread(priced.fairSpread);
            response_builder.add_fair_upfront(priced.fairUpfront);
            response_builder.add_default_leg_npv(priced.defaultLegNpv);
            response_builder.add_premium_leg_npv(priced.premiumLegNpv);

            cds_vector.push_back(response_builder.Finish());
        }
        catch (const std::exception &e)
        {
            // Create error response
            auto error_msg = builder->CreateString(std::string("CDS pricing error: ") + e.what());
            auto error = quantra::CreateError(*builder, error_msg);
            CDSValuesBuilder response_builder(*builder);
            response_builder.add_error(error);
            cds_vector.push_back(response_builder.Finish());
        }
    }

    // Build final response
    auto cds_list = builder->CreateVector(cds_vector);
    PriceCDSResponseBuilder response_builder(*builder);
    response_builder.add_cds_list(cds_list);

    return response_builder.Finish();
}
