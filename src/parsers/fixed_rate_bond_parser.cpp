#include "fixed_rate_bond_parser.h"

std::shared_ptr<QuantLib::Bond> FixedRateBondParser::parse(const quantra::FixedRateBond *bond)
{
    if (bond == NULL)
        QUANTRA_INVALID_ARGUMENT("Fixed Rate Bond not found");

    ScheduleParser schedule_parser = ScheduleParser();

    if (!bond->accrual_day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("FixedRateBond.accrual_day_counter is required");
    if (!bond->payment_convention().has_value())
        QUANTRA_INVALID_ARGUMENT("FixedRateBond.payment_convention is required");

    auto schedule = schedule_parser.parse(bond->schedule());
    auto dayCounter = DayCounterToQL(bond->accrual_day_counter().value());
    auto paymentConvention = ConventionToQL(bond->payment_convention().value());
    auto issueDate = DateToQL(bond->issue_date()->str());

    // Optional per-period notionals: present => amortizing/step-up bond.
    std::vector<double> notionals;
    const bool amortizing = quantra::parseOptionalNotionals(
        bond->notionals(), schedule->size() - 1, "FixedRateBond", notionals);

    if (amortizing) {
        // AmortizingFixedRateBond carries the outstanding notional per period;
        // the redemption cashflows are derived from the notional decrements at
        // `redemption` percent (matching the constant bond's redemption field).
        return std::make_shared<QuantLib::AmortizingFixedRateBond>(
            bond->settlement_days(),
            std::vector<Real>(notionals.begin(), notionals.end()),
            *schedule,
            std::vector<Rate>(1, bond->rate()),
            dayCounter,
            paymentConvention,
            issueDate,
            QuantLib::Period(),
            QuantLib::Calendar(),
            QuantLib::Unadjusted,
            false,
            std::vector<Real>(1, bond->redemption()));
    }

    return std::make_shared<QuantLib::FixedRateBond>(
        bond->settlement_days(),
        bond->face_amount(),
        *schedule,
        std::vector<Rate>(1, bond->rate()),
        dayCounter,
        paymentConvention,
        bond->redemption(),
        issueDate);
}
