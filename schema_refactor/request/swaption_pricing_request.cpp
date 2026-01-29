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

    // Build term structures map (curves bootstrapped once)
    auto curves = pricing->curves;
    std::map<std::string, std::shared_ptr<RelinkableHandle<YieldTermStructure>>> term_structures;

    for (auto it = curves->begin(); it != curves->end(); it++)
    {
        auto term_structure_handle = std::make_shared<RelinkableHandle<YieldTermStructure>>();
        std::shared_ptr<YieldTermStructure> term_structure = term_structure_parser.parse(*it);
        term_structure_handle->linkTo(term_structure);
        term_structures.insert(std::make_pair(it->id()->str(), term_structure_handle));
    }

    // Build swaption volatility surfaces map (built once)
    // Note: For swaptions we need SwaptionVolatilityStructure, not OptionletVolatilityStructure
    std::map<std::string, std::shared_ptr<QuantLib::SwaptionVolatilityStructure>> swaption_vol_surfaces;

    if (pricing->volatilities)
    {
        for (auto it = pricing->volatilities->begin(); it != pricing->volatilities->end(); it++)
        {
            // Build swaption vol structure from the FlatBuffers data
            double constantVol = it->constant_vol();
            QuantLib::VolatilityType volType = (it->volatility_type() == quantra::enums::VolatilityType_Normal) 
                ? QuantLib::Normal 
                : QuantLib::ShiftedLognormal;

            auto swaptionVol = std::make_shared<QuantLib::ConstantSwaptionVolatility>(
                as_of_date,
                CalendarToQL(it->calendar()),
                ConventionToQL(it->business_day_convention()),
                constantVol,
                DayCounterToQL(it->day_counter()),
                volType
            );
            swaption_vol_surfaces.insert(std::make_pair(it->id()->str(), swaptionVol));
        }
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

        // Get volatility surface by ID
        auto volatility_it = swaption_vol_surfaces.find(it->volatility()->str());
        if (volatility_it == swaption_vol_surfaces.end())
        {
            QUANTRA_ERROR("Volatility surface not found: " + it->volatility()->str());
        }
        QuantLib::Handle<QuantLib::SwaptionVolatilityStructure> volHandle(volatility_it->second);

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
        response_builder.add_implied_volatility(volatility_it->second->volatility(0.0, 0.0, 0.0));
        response_builder.add_atm_forward(0.0);
        response_builder.add_annuity(0.0);
        response_builder.add_delta(0.0);
        response_builder.add_vega(0.0);

        swaptions_vector.push_back(response_builder.Finish());
    }

    // Build final response
    auto swaptions = builder->CreateVector(swaptions_vector);
    PriceSwaptionResponseBuilder response_builder(*builder);
    response_builder.add_swaptions(swaptions);

    return response_builder.Finish();
}
