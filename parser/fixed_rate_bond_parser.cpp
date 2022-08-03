#include "fixed_rate_bond_parser.h"

std::shared_ptr<QuantLib::FixedRateBond> FixedRateBondParser::parse(const quantra::FixedRateBond *bond)
{
    if (bond == NULL)
        QUANTRA_ERROR("Fixed Rate Bond not found");

    ScheduleParser schedule_parser = ScheduleParser();

    return std::make_shared<QuantLib::FixedRateBond>(
        bond->settlement_days(),
        bond->face_amount(),
        *schedule_parser.parse(bond->schedule()),
        std::vector<Rate>(1, bond->rate()),
        DayCounterToQL(bond->accrual_day_counter()),
        ConventionToQL(bond->payment_convention()),
        bond->redemption(),
        DateToQL(bond->issue_date()->str()));
}