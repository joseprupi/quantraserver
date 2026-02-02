#include "index_parser.h"

std::shared_ptr<QuantLib::IborIndex> IndexParser::parse(const quantra::Index *index)
{
    if (index == NULL)
        QUANTRA_ERROR("Index not found");

    QuantLib::IndexManager::instance().clearHistories();

    std::shared_ptr<QuantLib::IborIndex> ibor_index(
        new QuantLib::IborIndex(
            "index",
            index->period_number() * TimeUnitToQL(index->period_time_unit()),
            index->settlement_days(),
            QuantLib::EURCurrency(),
            CalendarToQL(index->calendar()),
            ConventionToQL(index->business_day_convention()),
            index->end_of_month(),
            DayCounterToQL(index->day_counter()),
            this->term_structure));

    auto fixings = index->fixings();

    // Only iterate if fixings is not null
    if (fixings != NULL)
    {
        for (int i = 0; i < fixings->size(); i++)
        {
            auto fixing = fixings->Get(i);
            ibor_index->addFixing(DateToQL(fixing->date()->str()), fixing->rate());
        }
    }

    return ibor_index;
}

void IndexParser::link_term_structure(std::shared_ptr<YieldTermStructure> term_structure)
{
    this->term_structure.linkTo(term_structure);
}