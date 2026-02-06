#include <ql/quantlib.hpp>

#include "enums_generated.h"
#include "common.h"

#ifndef ENUMS_H
#define ENUMS_H

QuantLib::TimeUnit TimeUnitToQL(const quantra::enums::TimeUnit timeUnit);
QuantLib::Calendar CalendarToQL(const quantra::enums::Calendar calendar);
QuantLib::BusinessDayConvention ConventionToQL(const quantra::enums::BusinessDayConvention dayConvention);
QuantLib::DayCounter DayCounterToQL(const quantra::enums::DayCounter dayCounter);
QuantLib::Frequency FrequencyToQL(const quantra::enums::Frequency frequency);
QuantLib::DateGeneration::Rule DateGenerationToQL(const quantra::enums::DateGenerationRule dateGeneration);
QuantLib::Compounding CompoundingToQL(const quantra::enums::Compounding compounding);

#endif //ENUMS_H
