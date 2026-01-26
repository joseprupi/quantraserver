#include "common_parser.h"

std::shared_ptr<QuantLib::Schedule> ScheduleParser::parse(const quantra::Schedule *schedule)
{
    if (schedule == NULL)
        QUANTRA_ERROR("Schedule not found");

    return std::make_shared<QuantLib::Schedule>(
        DateToQL(schedule->effective_date()->str()),
        DateToQL(schedule->termination_date()->str()),
        QuantLib::Period(FrequencyToQL(schedule->frequency())),
        CalendarToQL(schedule->calendar()),
        ConventionToQL(schedule->convention()),
        ConventionToQL(schedule->termination_date_convention()),
        DateGenerationToQL(schedule->date_generation_rule()),
        schedule->end_of_month());
}

std::shared_ptr<YieldStruct> YieldParser::parse(const quantra::Yield *yield)
{

    if (yield == NULL)
        QUANTRA_ERROR("Yield not found");

    return std::make_shared<YieldStruct>(
        YieldStruct{
            DayCounterToQL(yield->day_counter()),
            CompoundingToQL(yield->compounding()),
            FrequencyToQL(yield->frequency())});
}

std::shared_ptr<PricingStruct> PricingParser::parse(const quantra::Pricing *pricing)
{
    if (pricing == NULL)
        QUANTRA_ERROR("Pricing not found");
    
    if (!pricing->as_of_date())
        QUANTRA_ERROR("as_of_date is required");
    
    if (!pricing->settlement_date())
        QUANTRA_ERROR("settlement_date is required");

    return std::make_shared<PricingStruct>(
        PricingStruct{
            pricing->as_of_date()->str(),
            pricing->settlement_date()->str(),
            pricing->curves(),
            pricing->bond_pricing_details(),
            pricing->bond_pricing_flows(),
            pricing->coupon_pricers()});
}