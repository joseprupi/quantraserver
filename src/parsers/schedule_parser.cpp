#include "schedule_parser.h"

#include "error.h"

namespace {

// Cap on the implied number of schedule periods (span / period length).
// Without it a tiny request (e.g. 1901->2199 Daily, ~108k dates) pins the
// single-threaded worker for the full gRPC deadline. Kept consistent with
// the range-grid cap in grid_utils (BuildRangeGrid default maxPoints=50000);
// comfortably above any legitimate instrument (50y weekly ~ 2600 periods).
constexpr long kMaxSchedulePeriods = 50000;

// Conservative (lower-bound) days per period so the estimate never
// over-rejects: Months uses 28, Years uses 365.
long approxDaysPerPeriod(const QuantLib::Period &period)
{
    switch (period.units())
    {
    case QuantLib::Days:
        return period.length();
    case QuantLib::Weeks:
        return 7L * period.length();
    case QuantLib::Months:
        return 28L * period.length();
    case QuantLib::Years:
        return 365L * period.length();
    default:
        return 0; // unknown unit: skip the cap, let QuantLib validate
    }
}

} // namespace

std::shared_ptr<QuantLib::Schedule> ScheduleParser::parse(const quantra::Schedule *schedule)
{
    if (schedule == NULL)
        QUANTRA_INVALID_ARGUMENT("Schedule not found");
    if (!schedule->effective_date())
        QUANTRA_INVALID_ARGUMENT("Schedule.effective_date is required");
    if (!schedule->termination_date())
        QUANTRA_INVALID_ARGUMENT("Schedule.termination_date is required");
    if (!schedule->calendar().has_value())
        QUANTRA_INVALID_ARGUMENT("Schedule.calendar is required");
    if (!schedule->frequency().has_value())
        QUANTRA_INVALID_ARGUMENT("Schedule.frequency is required");
    if (!schedule->convention().has_value())
        QUANTRA_INVALID_ARGUMENT("Schedule.convention is required");
    if (!schedule->termination_date_convention().has_value())
        QUANTRA_INVALID_ARGUMENT("Schedule.termination_date_convention is required");
    if (!schedule->date_generation_rule().has_value())
        QUANTRA_INVALID_ARGUMENT("Schedule.date_generation_rule is required");

    const QuantLib::Date effective = DateToQL(schedule->effective_date()->str());
    const QuantLib::Date termination = DateToQL(schedule->termination_date()->str());
    const QuantLib::Period period =
        FrequencyToPeriod(FrequencyToQL(schedule->frequency().value()));

    const long spanDays = termination - effective;
    const long periodDays = approxDaysPerPeriod(period);
    if (periodDays > 0 && spanDays > 0)
    {
        const long impliedPeriods = spanDays / periodDays;
        if (impliedPeriods > kMaxSchedulePeriods)
        {
            QUANTRA_INVALID_ARGUMENT(
                "Schedule too large: " + std::to_string(impliedPeriods) +
                " implied periods between " + schedule->effective_date()->str() +
                " and " + schedule->termination_date()->str() +
                " at the requested frequency (limit " +
                std::to_string(kMaxSchedulePeriods) + ")");
        }
    }

    // Optional stub-period control. Absent fields default to QuantLib::Date(),
    // which is exactly the Schedule constructor's own default, i.e. no stub.
    // Present fields are validated to lie
    // strictly inside (effective_date, termination_date) — QuantLib's own
    // requirement — so the caller gets a named 400 instead of an opaque error.
    QuantLib::Date firstDate;
    QuantLib::Date nextToLastDate;
    const bool hasFirst = schedule->first_date() != NULL;
    const bool hasNextToLast = schedule->next_to_last_date() != NULL;

    if (hasFirst)
    {
        firstDate = DateToQL(schedule->first_date()->str());
        if (firstDate <= effective || firstDate >= termination)
        {
            QUANTRA_INVALID_ARGUMENT(
                "Schedule.first_date (" + schedule->first_date()->str() +
                ") must lie strictly between effective_date (" +
                schedule->effective_date()->str() + ") and termination_date (" +
                schedule->termination_date()->str() + ")");
        }
    }

    if (hasNextToLast)
    {
        nextToLastDate = DateToQL(schedule->next_to_last_date()->str());
        if (nextToLastDate <= effective || nextToLastDate >= termination)
        {
            QUANTRA_INVALID_ARGUMENT(
                "Schedule.next_to_last_date (" + schedule->next_to_last_date()->str() +
                ") must lie strictly between effective_date (" +
                schedule->effective_date()->str() + ") and termination_date (" +
                schedule->termination_date()->str() + ")");
        }
    }

    if (hasFirst && hasNextToLast && firstDate > nextToLastDate)
    {
        QUANTRA_INVALID_ARGUMENT(
            "Schedule.first_date (" + schedule->first_date()->str() +
            ") must be on or before next_to_last_date (" +
            schedule->next_to_last_date()->str() + ")");
    }

    // With a stub date present, some DateGeneration rules (e.g. Zero) reject the
    // firstDate/nextToLastDate arguments outright. QuantLib signals this by
    // throwing; without a guard that raw error maps to an opaque 500. Surface it
    // as a named 400 carrying QuantLib's reason. Requests with no stub dates
    // take the unwrapped path below.
    if (hasFirst || hasNextToLast)
    {
        try
        {
            return std::make_shared<QuantLib::Schedule>(
                effective,
                termination,
                period,
                CalendarToQL(schedule->calendar().value()),
                ConventionToQL(schedule->convention().value()),
                ConventionToQL(schedule->termination_date_convention().value()),
                DateGenerationToQL(schedule->date_generation_rule().value()),
                schedule->end_of_month(),
                firstDate,
                nextToLastDate);
        }
        catch (const QuantLib::Error &e)
        {
            QUANTRA_INVALID_ARGUMENT(
                std::string("Schedule rejected by QuantLib for the requested "
                            "stub dates / date-generation rule: ") + e.what());
        }
    }

    return std::make_shared<QuantLib::Schedule>(
        effective,
        termination,
        period,
        CalendarToQL(schedule->calendar().value()),
        ConventionToQL(schedule->convention().value()),
        ConventionToQL(schedule->termination_date_convention().value()),
        DateGenerationToQL(schedule->date_generation_rule().value()),
        schedule->end_of_month());
}
