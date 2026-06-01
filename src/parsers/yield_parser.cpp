#include "yield_parser.h"

#include "error.h"

std::shared_ptr<YieldStruct> YieldParser::parse(const quantra::Yield *yield)
{
    if (yield == NULL)
        QUANTRA_INVALID_ARGUMENT("Yield not found");

    return std::make_shared<YieldStruct>(
        YieldStruct{
            DayCounterToQL(yield->day_counter()),
            CompoundingToQL(yield->compounding()),
            FrequencyToQL(yield->frequency())});
}
