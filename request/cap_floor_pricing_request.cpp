#include "cap_floor_pricing_request.h"
#include <ql/cashflows/iborcoupon.hpp>

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceCapFloorResponse> CapFloorPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceCapFloorRequest *request) const
{
    // Parse pricing information
    PricingParser pricing_parser;
    TermStructureParser term_structure_parser;
    CapFloorParser cap_floor_parser;
    VolatilityParser volatility_parser;

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

    // Process each Cap/Floor
    auto cap_floor_pricings = request->cap_floors();
    std::vector<flatbuffers::Offset<CapFloorResponse>> cap_floors_vector;

    for (auto it = cap_floor_pricings->begin(); it != cap_floor_pricings->end(); it++)
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

        // Parse volatility
        auto volStructure = volatility_parser.parse(it->volatility());
        QuantLib::Handle<QuantLib::OptionletVolatilityStructure> volHandle(volStructure);

        // Link forwarding curve and parse Cap/Floor
        cap_floor_parser.linkForwardingTermStructure(forwarding_curve_it->second->currentLink());
        auto capFloor = cap_floor_parser.parse(it->cap_floor());

        // Set pricing engine (Black model)
        auto engine = std::make_shared<BlackCapFloorEngine>(
            *discounting_curve_it->second,
            volHandle
        );
        capFloor->setPricingEngine(engine);

        // Calculate results
        double npv = capFloor->NPV();
        double atmRate = capFloor->atmRate(*discounting_curve_it->second->currentLink());

        std::cout << "CapFloor NPV: " << npv << ", ATM Rate: " << atmRate * 100 << "%" << std::endl;

        // Build caplet/floorlet details if requested
        std::vector<flatbuffers::Offset<CapFloorLet>> capfloorlets_vector;

        if (it->include_details())
        {
            const Leg& leg = capFloor->floatingLeg();
            auto discountCurve = discounting_curve_it->second->currentLink();

            for (size_t i = 0; i < leg.size(); i++)
            {
                auto coupon = std::dynamic_pointer_cast<IborCoupon>(leg[i]);
                if (coupon && !coupon->hasOccurred(as_of_date))
                {
                    std::ostringstream os_payment, os_start, os_end, os_fixing;
                    os_payment << QuantLib::io::iso_date(coupon->date());
                    os_start << QuantLib::io::iso_date(coupon->accrualStartDate());
                    os_end << QuantLib::io::iso_date(coupon->accrualEndDate());
                    os_fixing << QuantLib::io::iso_date(coupon->fixingDate());

                    auto payment_date = builder->CreateString(os_payment.str());
                    auto accrual_start = builder->CreateString(os_start.str());
                    auto accrual_end = builder->CreateString(os_end.str());
                    auto fixing_date = builder->CreateString(os_fixing.str());

                    double discount = discountCurve->discount(coupon->date());
                    double forwardRate = coupon->indexFixing();

                    // Get individual optionlet price (approximate - full calculation would need more work)
                    double optionletPrice = 0.0; // Simplified - would need per-optionlet pricing

                    CapFloorLetBuilder let_builder(*builder);
                    let_builder.add_payment_date(payment_date);
                    let_builder.add_accrual_start_date(accrual_start);
                    let_builder.add_accrual_end_date(accrual_end);
                    let_builder.add_fixing_date(fixing_date);
                    let_builder.add_strike(it->cap_floor()->strike());
                    let_builder.add_forward_rate(forwardRate);
                    let_builder.add_discount(discount);
                    let_builder.add_price(optionletPrice);

                    capfloorlets_vector.push_back(let_builder.Finish());
                }
            }
        }

        auto capfloorlets = builder->CreateVector(capfloorlets_vector);

        // Build Cap/Floor response
        CapFloorResponseBuilder response_builder(*builder);
        response_builder.add_npv(npv);
        response_builder.add_atm_rate(atmRate);
        response_builder.add_implied_volatility(0.0); // Would need market price to imply vol

        if (it->include_details())
        {
            response_builder.add_cap_floor_lets(capfloorlets);
        }

        cap_floors_vector.push_back(response_builder.Finish());
    }

    // Build final response
    auto cap_floors = builder->CreateVector(cap_floors_vector);
    PriceCapFloorResponseBuilder response_builder(*builder);
    response_builder.add_cap_floors(cap_floors);

    return response_builder.Finish();
}