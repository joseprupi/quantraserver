#include "cds_pricing_request.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceCDSResponse> CDSPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceCDSRequest *request) const
{
    // Parse pricing information
    PricingParser pricing_parser;
    TermStructureParser term_structure_parser;
    CDSParser cds_parser;
    CreditCurveParser credit_curve_parser;

    auto pricing = pricing_parser.parse(request->pricing());

    // Set evaluation date
    QuantLib::Date as_of_date = DateToQL(pricing->as_of_date);
    QuantLib::Settings::instance().evaluationDate() = as_of_date;

    // Build all yield curves
    auto curves = pricing->curves;
    std::map<std::string, std::shared_ptr<RelinkableHandle<YieldTermStructure>>> term_structures;

    for (auto it = curves->begin(); it != curves->end(); it++)
    {
        auto term_structure_handle = std::make_shared<RelinkableHandle<YieldTermStructure>>();
        std::shared_ptr<YieldTermStructure> term_structure = term_structure_parser.parse(*it);
        term_structure_handle->linkTo(term_structure);
        term_structures.insert(std::make_pair(it->id()->str(), term_structure_handle));
    }

    // Process each CDS
    auto cds_pricings = request->cds_list();
    std::vector<flatbuffers::Offset<CDSValues>> cds_vector;

    for (auto it = cds_pricings->begin(); it != cds_pricings->end(); it++)
    {
        try
        {
            // Lookup discounting curve by ID
            auto discounting_curve_it = term_structures.find(it->discounting_curve()->str());
            if (discounting_curve_it == term_structures.end())
            {
                // Create error response
                auto error_msg = builder->CreateString("Discounting curve not found: " + it->discounting_curve()->str());
                auto error = quantra::CreateError(*builder, error_msg);
                CDSValuesBuilder response_builder(*builder);
                response_builder.add_error(error);
                cds_vector.push_back(response_builder.Finish());
                continue;
            }

            // Build credit curve from the embedded credit_curve
            auto credit_curve_data = it->credit_curve();
            double recoveryRate = credit_curve_data->recovery_rate();

            auto credit_curve = credit_curve_parser.parse(
                credit_curve_data,
                as_of_date,
                QuantLib::Handle<QuantLib::YieldTermStructure>(discounting_curve_it->second->currentLink()));

            QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure> creditHandle(credit_curve);

            // Parse the CDS instrument
            auto cds = cds_parser.parse(it->cds());

            // Set pricing engine
            auto engine = std::make_shared<QuantLib::MidPointCdsEngine>(
                creditHandle,
                recoveryRate,
                *discounting_curve_it->second);
            cds->setPricingEngine(engine);

            // Calculate results
            double npv = cds->NPV();
            double fairSpread = cds->fairSpread();
            double fairUpfront = cds->fairUpfront();
            double defaultLegNPV = cds->defaultLegNPV();
            double premiumLegNPV = cds->couponLegNPV();

            std::cout << "CDS NPV: " << npv << ", Fair Spread: " << fairSpread * 10000 << " bps" << std::endl;

            // Build response
            CDSValuesBuilder response_builder(*builder);
            response_builder.add_npv(npv);
            response_builder.add_fair_spread(fairSpread);
            response_builder.add_fair_upfront(fairUpfront);
            response_builder.add_default_leg_npv(defaultLegNPV);
            response_builder.add_premium_leg_npv(premiumLegNPV);

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
