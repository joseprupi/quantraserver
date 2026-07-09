#include "fixed_rate_bond_parser.h"

std::shared_ptr<QuantLib::FixedRateBond> FixedRateBondParser::parse(const quantra::FixedRateBond *bond)
{
    if (bond == NULL)
        QUANTRA_INVALID_ARGUMENT("Fixed Rate Bond not found");

    ScheduleParser schedule_parser = ScheduleParser();

    if (!bond->accrual_day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("FixedRateBond.accrual_day_counter is required");
    if (!bond->payment_convention().has_value())
        QUANTRA_INVALID_ARGUMENT("FixedRateBond.payment_convention is required");

    return std::make_shared<QuantLib::FixedRateBond>(
        bond->settlement_days(),
        bond->face_amount(),
        *schedule_parser.parse(bond->schedule()),
        std::vector<Rate>(1, bond->rate()),
        DayCounterToQL(bond->accrual_day_counter().value()),
        ConventionToQL(bond->payment_convention().value()),
        bond->redemption(),
        DateToQL(bond->issue_date()->str()));
}