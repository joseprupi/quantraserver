#include "fra_pricing_request.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceFRAResponse> FRAPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceFRARequest *request) const
{
    // Parse pricing information
    PricingParser pricing_parser;
    TermStructureParser term_structure_parser;
    FRAParser fra_parser;

    auto pricing = pricing_parser.parse(request->pricing());

    // Set evaluation date
    Date as_of_date = DateToQL(pricing->as_of_date);
    Settings::instance().evaluationDate() = as_of_date;

    // Build term structures map
    auto curves = pricing->curves;
    std::map<std::string, std::shared_ptr<RelinkableHandle<YieldTermStructure>>> term_structures;

    for (auto it = curves->begin(); it != curves->end(); it++)
    {
        auto term_structure_handle = std::make_shared<RelinkableHandle<YieldTermStructure>>();
        std::shared_ptr<YieldTermStructure> term_structure = term_structure_parser.parse(*it);
        term_structure_handle->linkTo(term_structure);
        term_structures.insert(std::make_pair(it->id()->str(), term_structure_handle));
    }

    // Process each FRA
    auto fra_pricings = request->fras();
    std::vector<flatbuffers::Offset<FRAResponse>> fras_vector;

    for (auto it = fra_pricings->begin(); it != fra_pricings->end(); it++)
    {
        // Get discounting curve
        auto discounting_curve_it = term_structures.find(it->discounting_curve()->str());
        if (discounting_curve_it == term_structures.end())
        {
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());
        }

        // Get forwarding curve
        auto forwarding_curve_it = term_structures.find(it->forwarding_curve()->str());
        if (forwarding_curve_it == term_structures.end())
        {
            QUANTRA_ERROR("Forwarding curve not found: " + it->forwarding_curve()->str());
        }

        // Link forwarding curve and parse FRA
        fra_parser.linkForwardingTermStructure(forwarding_curve_it->second->currentLink());
        auto fra = fra_parser.parse(it->fra());

        // Calculate results
        double npv = fra->NPV();
        double forwardRate = fra->forwardRate();
        double spotValue = fra->spotValue();
        Date settlementDate = fra->valueDate();

        std::cout << "FRA NPV: " << npv << ", Forward Rate: " << forwardRate * 100 << "%" << std::endl;

        // Format settlement date
        std::ostringstream os_settlement;
        os_settlement << QuantLib::io::iso_date(settlementDate);
        auto settlement_date_str = builder->CreateString(os_settlement.str());

        // Build FRA response
        FRAResponseBuilder fra_response_builder(*builder);
        fra_response_builder.add_npv(npv);
        fra_response_builder.add_forward_rate(forwardRate);
        fra_response_builder.add_spot_value(spotValue);
        fra_response_builder.add_settlement_date(settlement_date_str);

        fras_vector.push_back(fra_response_builder.Finish());
    }

    // Build final response
    auto fras = builder->CreateVector(fras_vector);
    PriceFRAResponseBuilder response_builder(*builder);
    response_builder.add_fras(fras);

    return response_builder.Finish();
}
