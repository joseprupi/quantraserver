#include "floating_rate_bond_parser.h"

std::shared_ptr<QuantLib::FloatingRateBond> FloatingRateBondParser::parse(
    const quantra::FloatingRateBond *bond,
    const quantra::IndexRegistry& indices)
{
    if (bond == NULL)
        QUANTRA_INVALID_ARGUMENT("Floating Rate Bond not found");

    if (!bond->index() || !bond->index()->id())
        QUANTRA_INVALID_ARGUMENT("FloatingRateBond index.id is required");

    ScheduleParser schedule_parser;

    // Resolve index from registry and clone with forwarding curve
    std::string indexId = bond->index()->id()->str();
    auto iborIndex = indices.getIborWithCurve(indexId, forwarding_term_structure_);

    return std::make_shared<QuantLib::FloatingRateBond>(
        bond->settlement_days(),
        bond->face_amount(),
        *schedule_parser.parse(bond->schedule()),
        iborIndex,
        DayCounterToQL(bond->accrual_day_counter()),
        ConventionToQL(bond->payment_convention()),
        bond->fixing_days(),
        std::vector<Real>(1, 1.0),
        std::vector<Spread>(1, bond->spread()),
        std::vector<Rate>(),
        std::vector<Rate>(),
        bond->in_arrears(),
        bond->redemption(),
        DateToQL(bond->issue_date()->str()));
}

void FloatingRateBondParser::linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure)
{
    forwarding_term_structure_.linkTo(term_structure);
}
