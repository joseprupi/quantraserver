#include "cap_floor_pricing_request.h"
#include <ql/cashflows/iborcoupon.hpp>

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
        auto dIt = reg.curves.find(it->discounting_curve()->str());
        if (dIt == reg.curves.end())
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());

        auto fIt = reg.curves.find(it->forwarding_curve()->str());
        if (fIt == reg.curves.end())
            QUANTRA_ERROR("Forwarding curve not found: " + it->forwarding_curve()->str());

        auto vIt = reg.optionletVols.find(it->volatility()->str());
        if (vIt == reg.optionletVols.end())
            QUANTRA_ERROR("Optionlet vol not found: " + it->volatility()->str());

        auto mIt = reg.models.find(it->model()->str());
        if (mIt == reg.models.end())
            QUANTRA_ERROR("Model not found: " + it->model()->str());

        cap_floor_parser.linkForwardingTermStructure(fIt->second->currentLink());
        auto capFloor = cap_floor_parser.parse(it->cap_floor(), reg.indices);

        Handle<YieldTermStructure> discountCurve(dIt->second->currentLink());
        auto engine = engineFactory.makeCapFloorEngine(mIt->second, discountCurve, vIt->second);
        capFloor->setPricingEngine(engine);

        double npv = capFloor->NPV();
        double atmRate = capFloor->atmRate(*dIt->second->currentLink());

        std::cout << "CapFloor NPV: " << npv << ", ATM Rate: " << atmRate * 100 << "%" << std::endl;

        std::vector<flatbuffers::Offset<CapFloorLet>> capfloorlets_vector;

        if (it->include_details())
        {
            const Leg& leg = capFloor->floatingLeg();
            auto discountCurvePtr = dIt->second->currentLink();

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

                    double discount = discountCurvePtr->discount(coupon->date());

                    CapFloorLetBuilder let_builder(*builder);
                    let_builder.add_payment_date(payment_date);
                    let_builder.add_accrual_start_date(accrual_start);
                    let_builder.add_accrual_end_date(accrual_end);
                    let_builder.add_fixing_date(fixing_date);
                    let_builder.add_forward_rate(coupon->indexFixing());
                    let_builder.add_discount(discount);

                    capfloorlets_vector.push_back(let_builder.Finish());
                }
            }
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
