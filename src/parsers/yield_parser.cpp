#include "yield_parser.h"

#include "error.h"

std::shared_ptr<YieldStruct> YieldParser::parse(const quantra::Yield *yield)
{
    if (yield == NULL)
        QUANTRA_INVALID_ARGUMENT("Yield not found");

    if (!yield->day_counter().has_value())
        QUANTRA_INVALID_ARGUMENT("Yield.day_counter is required");
    if (!yield->compounding().has_value())
        QUANTRA_INVALID_ARGUMENT("Yield.compounding is required");
    if (!yield->frequency().has_value())
        QUANTRA_INVALID_ARGUMENT("Yield.frequency is required");

    return std::make_shared<YieldStruct>(
        YieldStruct{
            DayCounterToQL(yield->day_counter().value()),
            CompoundingToQL(yield->compounding().value()),
            FrequencyToQL(yield->frequency().value())});
}
