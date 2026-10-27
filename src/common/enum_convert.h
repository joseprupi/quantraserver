#include "date_convert.h"
#include "enums_domain.h"

#include "enums_generated.h"

#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/capfloor.hpp>
#include <ql/option.hpp>
#include <ql/quantlib.hpp>

#ifndef ENUM_CONVERT_H
#define ENUM_CONVERT_H

QuantLib::TimeUnit TimeUnitToQL(const quantra::enums::TimeUnit timeUnit);
QuantLib::Calendar CalendarToQL(const quantra::enums::Calendar calendar);
QuantLib::BusinessDayConvention ConventionToQL(
    const quantra::enums::BusinessDayConvention dayConvention);

// Reverse maps (QuantLib -> FlatBuffers enum). Centralized here per the
// enum-conversion single-source rule.
quantra::enums::TimeUnit TimeUnitToFb(QuantLib::TimeUnit timeUnit);
quantra::enums::VolatilityType VolatilityTypeToFb(
    QuantLib::VolatilityType type, double displacement);
quantra::enums::EquitySettlementType EquitySettlementTypeToFb(QuantLib::Settlement::Type type);
QuantLib::DayCounter DayCounterToQL(const quantra::enums::DayCounter dayCounter);
QuantLib::Frequency FrequencyToQL(const quantra::enums::Frequency frequency);
QuantLib::Period FrequencyToPeriod(QuantLib::Frequency frequency);
QuantLib::DateGeneration::Rule DateGenerationToQL(
    const quantra::enums::DateGenerationRule dateGeneration);
QuantLib::Compounding CompoundingToQL(const quantra::enums::Compounding compounding);
QuantLib::RateAveraging::Type RateAveragingToQL(const quantra::enums::RateAveragingType averaging);
QuantLib::Settlement::Method SettlementMethodToQL(const quantra::enums::SettlementMethod method);
QuantLib::ZeroCouponSwap::Type ZeroCouponSwapTypeToQL(const quantra::enums::SwapType swapType);
QuantLib::ZeroCouponInflationSwap::Type ZeroCouponInflationSwapTypeToQL(
    const quantra::enums::SwapType swapType);
QuantLib::YearOnYearInflationSwap::Type YearOnYearInflationSwapTypeToQL(
    const quantra::enums::SwapType swapType);
QuantLib::CPI::InterpolationType CPIInterpolationToQL(
    const quantra::enums::CPIInterpolationType interpolation);
QuantLib::Protection::Side ProtectionSideToQL(const quantra::enums::ProtectionSide side);
QuantLib::Position::Type FRATypeToQL(const quantra::enums::FRAType fraType);
QuantLib::CapFloor::Type CapFloorTypeToQL(const quantra::enums::CapFloorType capFloorType);
QuantLib::Option::Type EquityOptionTypeToQL(const quantra::enums::EquityOptionType optionType);
QuantLib::Barrier::Type EquityBarrierTypeToQL(const quantra::enums::EquityBarrierType barrierType);
QuantLib::Settlement::Type EquitySettlementTypeToQL(
    const quantra::enums::EquitySettlementType settlement);
/// FX forward direction: the wire enum has no QuantLib counterpart, so it maps
/// to the FB-free domain mirror the evaluator consumes.
quantra::FxDirectionKind FxDirectionToDomain(const quantra::enums::FxDirection direction);

#endif //ENUM_CONVERT_H
