#include "year_on_year_inflation_swap_parser.h"

#include "common.h"
#include "common_parser.h"
#include "enums.h"
#include "error.h"

namespace {

QuantLib::CPI::InterpolationType toQlInterpolation(quantra::enums::CPIInterpolationType interpolation) {
    switch (interpolation) {
        case quantra::enums::CPIInterpolationType_AsIndex:
            return QuantLib::CPI::AsIndex;
        case quantra::enums::CPIInterpolationType_Flat:
            return QuantLib::CPI::Flat;
        case quantra::enums::CPIInterpolationType_Linear:
            return QuantLib::CPI::Linear;
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported CPI interpolation type");
    return QuantLib::CPI::AsIndex;
}

} // namespace

std::shared_ptr<QuantLib::YearOnYearInflationSwap> YearOnYearInflationSwapParser::parse(
    const quantra::YearOnYearInflationSwap* swap,
    const std::shared_ptr<QuantLib::YoYInflationIndex>& inflationIndex) {
    if (!swap) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap not found");
    }
    if (!swap->fixed_schedule()) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap fixed_schedule not found");
    }
    if (!swap->yoy_schedule()) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap yoy_schedule not found");
    }
    if (!swap->observation_lag()) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap observation_lag not found");
    }
    if (!swap->inflation_index_id() || swap->inflation_index_id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap inflation_index_id is required");
    }
    if (!inflationIndex) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap inflation index not available");
    }

    QuantLib::YearOnYearInflationSwap::Type swapType;
    switch (swap->swap_type()) {
        case quantra::enums::SwapType_Payer:
            swapType = QuantLib::YearOnYearInflationSwap::Payer;
            break;
        case quantra::enums::SwapType_Receiver:
            swapType = QuantLib::YearOnYearInflationSwap::Receiver;
            break;
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid year-on-year inflation swap type");
    }

    ScheduleParser scheduleParser;
    auto fixedSchedule = scheduleParser.parse(swap->fixed_schedule());
    auto yoySchedule = scheduleParser.parse(swap->yoy_schedule());

    return std::make_shared<QuantLib::YearOnYearInflationSwap>(
        swapType,
        swap->notional(),
        *fixedSchedule,
        swap->fixed_rate(),
        DayCounterToQL(swap->fixed_day_counter()),
        *yoySchedule,
        inflationIndex,
        QuantLib::Period(
            swap->observation_lag()->n(),
            TimeUnitToQL(swap->observation_lag()->unit())),
        toQlInterpolation(swap->observation_interpolation()),
        swap->spread(),
        DayCounterToQL(swap->yoy_day_counter()),
        CalendarToQL(swap->payment_calendar()),
        ConventionToQL(swap->payment_convention()));
}
