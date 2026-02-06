#include "cap_floor_parser.h"
#include <ql/cashflows/couponpricer.hpp>

std::shared_ptr<QuantLib::CapFloor> CapFloorParser::parse(
    const quantra::CapFloor *capFloor,
    const quantra::IndexRegistry& indices)
{
    if (capFloor == NULL)
        QUANTRA_ERROR("CapFloor not found");

    if (capFloor->schedule() == NULL)
        QUANTRA_ERROR("CapFloor schedule not found");

    if (!capFloor->index() || !capFloor->index()->id())
        QUANTRA_ERROR("CapFloor index.id is required");

    // Parse schedule
    ScheduleParser scheduleParser;
    auto schedule = scheduleParser.parse(capFloor->schedule());

    // Resolve index from registry and clone with forwarding curve
    std::string indexId = capFloor->index()->id()->str();
    auto iborIndex = indices.getIborWithCurve(indexId, forwarding_term_structure_);

    // Create the floating leg (IborLeg)
    Leg leg = IborLeg(*schedule, iborIndex)
        .withNotionals(capFloor->notional())
        .withPaymentDayCounter(DayCounterToQL(capFloor->day_counter()))
        .withPaymentAdjustment(ConventionToQL(capFloor->business_day_convention()));

    // Parse cap/floor type and create instrument
    std::shared_ptr<QuantLib::CapFloor> instrument;
    std::vector<Rate> strikes(1, capFloor->strike());

    switch (capFloor->cap_floor_type()) {
        case quantra::enums::CapFloorType_Cap:
            instrument = std::make_shared<QuantLib::Cap>(leg, strikes);
            break;
        case quantra::enums::CapFloorType_Floor:
            instrument = std::make_shared<QuantLib::Floor>(leg, strikes);
            break;
        case quantra::enums::CapFloorType_Collar:
            QUANTRA_ERROR("Collar not yet supported - use separate Cap and Floor");
            break;
        default:
            QUANTRA_ERROR("Invalid CapFloor type");
    }

    return instrument;
}

void CapFloorParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
