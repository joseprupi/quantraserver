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
std::shared_ptr<QuantLib::IborIndex> IborToQL(const quantra::enums::Ibor ibor);
QuantLib::DateGeneration::Rule DateGenerationToQL(const quantra::enums::DateGenerationRule dateGeneration);
QuantLib::Compounding CompoundingToQL(const quantra::enums::Compounding compounding);

// NOTE: OvernightIndex and IborFamily mapping is handled by IndexFactory
// (parser/index_factory.h) rather than global functions, since those
// require forwarding curve handles.

#endif //ENUMS_H
