#include "swaption_pricing_request.h"

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceSwaptionResponse> SwaptionPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceSwaptionRequest *request) const
{
    // Parse pricing information
    PricingParser pricing_parser;
    TermStructureParser term_structure_parser;
    SwaptionParser swaption_parser;

    auto pricing = pricing_parser.parse(request->pricing());

    // Set evaluation date
    QuantLib::Date as_of_date = DateToQL(pricing->as_of_date);
    QuantLib::Settings::instance().evaluationDate() = as_of_date;

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

    // Process each Swaption
    auto swaption_pricings = request->swaptions();
    std::vector<flatbuffers::Offset<SwaptionResponse>> swaptions_vector;

    for (auto it = swaption_pricings->begin(); it != swaption_pricings->end(); it++)
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

        // Parse volatility - create swaption vol structure from constant vol
        auto volInput = it->volatility();
        if (volInput == NULL)
            QUANTRA_ERROR("Volatility not found for swaption");

        double constantVol = volInput->constant_vol();
        QuantLib::VolatilityType volType = (volInput->volatility_type() == quantra::VolatilityType_Normal) 
            ? QuantLib::Normal 
            : QuantLib::ShiftedLognormal;

        auto swaptionVol = std::make_shared<QuantLib::ConstantSwaptionVolatility>(
            as_of_date,
            CalendarToQL(volInput->calendar()),
            ConventionToQL(volInput->business_day_convention()),
            constantVol,
            DayCounterToQL(volInput->day_counter()),
            volType
        );
        QuantLib::Handle<QuantLib::SwaptionVolatilityStructure> volHandle(swaptionVol);

        // Link forwarding curve and parse Swaption
        swaption_parser.linkForwardingTermStructure(forwarding_curve_it->second->currentLink());
        auto swaption = swaption_parser.parse(it->swaption());

        // Set pricing engine (Black model for European)
        auto engine = std::make_shared<QuantLib::BlackSwaptionEngine>(
            *discounting_curve_it->second,
            volHandle
        );
        swaption->setPricingEngine(engine);

        // Calculate results
        double npv = swaption->NPV();

        std::cout << "Swaption NPV: " << npv << std::endl;

        // Build Swaption response
        SwaptionResponseBuilder response_builder(*builder);
        response_builder.add_npv(npv);
        response_builder.add_implied_volatility(constantVol);  // We used this vol to price
        response_builder.add_atm_forward(0.0);  // Would need underlying swap rate
        response_builder.add_annuity(0.0);      // Would need swap annuity calculation
        response_builder.add_delta(0.0);        // Would need bump and reprice
        response_builder.add_vega(0.0);         // Would need bump and reprice

        swaptions_vector.push_back(response_builder.Finish());
    }

    // Build final response
    auto swaptions = builder->CreateVector(swaptions_vector);
    PriceSwaptionResponseBuilder response_builder(*builder);
    response_builder.add_swaptions(swaptions);

    return response_builder.Finish();
}