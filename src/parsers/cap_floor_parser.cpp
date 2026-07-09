#include "cap_floor_parser.h"
#include <ql/cashflows/couponpricer.hpp>

std::shared_ptr<QuantLib::CapFloor> CapFloorParser::parse(
    const quantra::CapFloor *capFloor,
    const quantra::IndexRegistry& indices)
{
    if (capFloor == NULL)
        QUANTRA_INVALID_ARGUMENT("CapFloor not found");

    if (capFloor->schedule() == NULL)
        QUANTRA_INVALID_ARGUMENT("CapFloor schedule not found");

    if (!capFloor->index() || !capFloor->index()->id())
        QUANTRA_INVALID_ARGUMENT("CapFloor index.id is required");

    if (!capFloor->day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("CapFloor.day_counter is required");
    if (!capFloor->business_day_convention().has_value())
        QUANTRA_INVALID_ARGUMENT("CapFloor.business_day_convention is required");

    // Parse schedule
    ScheduleParser scheduleParser;
    auto schedule = scheduleParser.parse(capFloor->schedule());

    // Resolve index from registry and clone with forwarding curve
    std::string indexId = capFloor->index()->id()->str();
    auto iborIndex = indices.getIborWithCurve(indexId, forwarding_term_structure_);

    // Create the floating leg (IborLeg)
    Leg leg = IborLeg(*schedule, iborIndex)
        .withNotionals(capFloor->notional())
        .withPaymentDayCounter(DayCounterToQL(capFloor->day_counter().value()))
        .withPaymentAdjustment(ConventionToQL(capFloor->business_day_convention().value()));

    // Parse cap/floor type and create instrument
    if (!capFloor->cap_floor_type().has_value())
        QUANTRA_INVALID_ARGUMENT("CapFloor.cap_floor_type is required");
    std::shared_ptr<QuantLib::CapFloor> instrument;
    std::vector<Rate> strikes(1, capFloor->strike());

    switch (capFloor->cap_floor_type().value()) {
        case quantra::enums::CapFloorType_Cap:
            instrument = std::make_shared<QuantLib::Cap>(leg, strikes);
            break;
        case quantra::enums::CapFloorType_Floor:
            instrument = std::make_shared<QuantLib::Floor>(leg, strikes);
            break;
        case quantra::enums::CapFloorType_Collar:
            QUANTRA_INVALID_ARGUMENT("Collar not yet supported - use separate Cap and Floor");
            break;
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid CapFloor type");
    }

    return instrument;
}

void CapFloorParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
