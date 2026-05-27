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
QuantLib::Period FrequencyToPeriod(QuantLib::Frequency frequency);
QuantLib::DateGeneration::Rule DateGenerationToQL(const quantra::enums::DateGenerationRule dateGeneration);
QuantLib::Compounding CompoundingToQL(const quantra::enums::Compounding compounding);
QuantLib::RateAveraging::Type RateAveragingToQL(const quantra::enums::RateAveragingType averaging);
QuantLib::Settlement::Method SettlementMethodToQL(const quantra::enums::SettlementMethod method);
QuantLib::VanillaSwap::Type SwapTypeToQL(const quantra::enums::SwapType swapType);
QuantLib::ZeroCouponInflationSwap::Type ZeroCouponInflationSwapTypeToQL(
    const quantra::enums::SwapType swapType);
QuantLib::YearOnYearInflationSwap::Type YearOnYearInflationSwapTypeToQL(
    const quantra::enums::SwapType swapType);
QuantLib::CPI::InterpolationType CPIInterpolationToQL(
    const quantra::enums::CPIInterpolationType interpolation);
QuantLib::Protection::Side ProtectionSideToQL(
    const quantra::enums::ProtectionSide side);

#endif //ENUMS_H
