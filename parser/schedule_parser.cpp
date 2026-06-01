#include "schedule_parser.h"

#include "error.h"

std::shared_ptr<QuantLib::Schedule> ScheduleParser::parse(const quantra::Schedule *schedule)
{
    if (schedule == NULL)
        QUANTRA_INVALID_ARGUMENT("Schedule not found");

    return std::make_shared<QuantLib::Schedule>(
        DateToQL(schedule->effective_date()->str()),
        DateToQL(schedule->termination_date()->str()),
        FrequencyToPeriod(FrequencyToQL(schedule->frequency())),
        CalendarToQL(schedule->calendar()),
        ConventionToQL(schedule->convention()),
        ConventionToQL(schedule->termination_date_convention()),
        DateGenerationToQL(schedule->date_generation_rule()),
        schedule->end_of_month());
}
