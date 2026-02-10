#include "ois_swap_parser.h"

std::shared_ptr<QuantLib::OvernightIndexedSwap> OisSwapParser::parse(
    const quantra::OisSwap *swap,
    const quantra::IndexRegistry& indices)
{
    if (swap == NULL)
        QUANTRA_ERROR("OisSwap not found");

    if (swap->fixed_leg() == NULL)
        QUANTRA_ERROR("OisSwap fixed_leg not found");

    if (swap->overnight_leg() == NULL)
        QUANTRA_ERROR("OisSwap overnight_leg not found");

    QuantLib::OvernightIndexedSwap::Type swapType;
    switch (swap->swap_type()) {
        case quantra::enums::SwapType_Payer:
            swapType = QuantLib::OvernightIndexedSwap::Payer;
            break;
        case quantra::enums::SwapType_Receiver:
            swapType = QuantLib::OvernightIndexedSwap::Receiver;
            break;
        default:
            QUANTRA_ERROR("Invalid swap type");
    }

    ScheduleParser scheduleParser;

    auto fixedLeg = swap->fixed_leg();
    if (fixedLeg->schedule() == NULL)
        QUANTRA_ERROR("OisSwap fixed_leg schedule not found");

    auto fixedSchedule = scheduleParser.parse(fixedLeg->schedule());
    double fixedNotional = fixedLeg->notional();
    double fixedRate = fixedLeg->rate();
    DayCounter fixedDayCounter = DayCounterToQL(fixedLeg->day_counter());

    auto overnightLeg = swap->overnight_leg();
    if (overnightLeg->schedule() == NULL)
        QUANTRA_ERROR("OisSwap overnight_leg schedule not found");

    if (!overnightLeg->index() || !overnightLeg->index()->id())
        QUANTRA_ERROR("OisSwap overnight_leg index.id is required");

    auto overnightSchedule = scheduleParser.parse(overnightLeg->schedule());
    double overnightNotional = overnightLeg->notional();
    double spread = overnightLeg->spread();
    std::string indexId = overnightLeg->index()->id()->str();
    auto overnightIndex = indices.getOvernightWithCurve(indexId, forwarding_term_structure_);

    BusinessDayConvention paymentConvention = ConventionToQL(overnightLeg->payment_convention());
    Calendar paymentCalendar = CalendarToQL(overnightLeg->payment_calendar());
    Integer paymentLag = overnightLeg->payment_lag();
    RateAveraging::Type averagingMethod = RateAveragingToQL(overnightLeg->averaging_method());

    Natural lookbackDays = overnightLeg->lookback_days() < 0
        ? QuantLib::Null<Natural>()
        : static_cast<Natural>(overnightLeg->lookback_days());
    Natural lockoutDays = static_cast<Natural>(overnightLeg->lockout_days());
    bool applyObservationShift = overnightLeg->apply_observation_shift();
    bool telescopicValueDates = overnightLeg->telescopic_value_dates();

    if (fixedNotional != overnightNotional) {
        std::cout << "Warning: Fixed and overnight notionals differ. Using fixed notional." << std::endl;
    }

    auto oisSwap = std::make_shared<QuantLib::OvernightIndexedSwap>(
        swapType,
        fixedNotional,
        *fixedSchedule,
        fixedRate,
        fixedDayCounter,
        *overnightSchedule,
        overnightIndex,
        spread,
        paymentLag,
        paymentConvention,
        paymentCalendar,
        telescopicValueDates,
        averagingMethod,
        lookbackDays,
        lockoutDays,
        applyObservationShift
    );

    return oisSwap;
}

void OisSwapParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
