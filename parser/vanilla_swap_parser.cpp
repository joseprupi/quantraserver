#include "vanilla_swap_parser.h"

std::shared_ptr<QuantLib::VanillaSwap> VanillaSwapParser::parse(
    const quantra::VanillaSwap *swap,
    const quantra::IndexRegistry& indices)
{
    if (swap == NULL)
        QUANTRA_ERROR("VanillaSwap not found");

    if (swap->fixed_leg() == NULL)
        QUANTRA_ERROR("VanillaSwap fixed_leg not found");

    if (swap->floating_leg() == NULL)
        QUANTRA_ERROR("VanillaSwap floating_leg not found");

    QuantLib::VanillaSwap::Type swapType;
    switch (swap->swap_type()) {
        case quantra::enums::SwapType_Payer:
            swapType = QuantLib::VanillaSwap::Payer;
            break;
        case quantra::enums::SwapType_Receiver:
            swapType = QuantLib::VanillaSwap::Receiver;
            break;
        default:
            QUANTRA_ERROR("Invalid swap type");
    }

    auto fixedLeg = swap->fixed_leg();
    if (fixedLeg->schedule() == NULL)
        QUANTRA_ERROR("VanillaSwap fixed_leg schedule not found");

    ScheduleParser scheduleParser;
    auto fixedSchedule = scheduleParser.parse(fixedLeg->schedule());

    double fixedNotional = fixedLeg->notional();
    double fixedRate = fixedLeg->rate();
    DayCounter fixedDayCounter = DayCounterToQL(fixedLeg->day_counter());

    // Parse floating leg
    auto floatingLeg = swap->floating_leg();
    if (floatingLeg->schedule() == NULL)
        QUANTRA_ERROR("VanillaSwap floating_leg schedule not found");

    if (!floatingLeg->index() || !floatingLeg->index()->id())
        QUANTRA_ERROR("VanillaSwap floating_leg index.id is required");

    auto floatingSchedule = scheduleParser.parse(floatingLeg->schedule());

    double floatingNotional = floatingLeg->notional();
    double spread = floatingLeg->spread();
    DayCounter floatingDayCounter = DayCounterToQL(floatingLeg->day_counter());

    // Resolve index from registry and clone with forwarding curve
    std::string indexId = floatingLeg->index()->id()->str();
    auto iborIndex = indices.getIborWithCurve(indexId, forwarding_term_structure_);

    if (fixedNotional != floatingNotional) {
        std::cout << "Warning: Fixed and floating notionals differ. Using fixed notional." << std::endl;
    }

    auto vanillaSwap = std::make_shared<QuantLib::VanillaSwap>(
        swapType,
        fixedNotional,
        *fixedSchedule,
        fixedRate,
        fixedDayCounter,
        *floatingSchedule,
        iborIndex,
        spread,
        floatingDayCounter
    );

    return vanillaSwap;
}

void VanillaSwapParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
