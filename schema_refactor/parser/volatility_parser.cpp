#include "volatility_parser.h"

std::shared_ptr<QuantLib::OptionletVolatilityStructure> VolatilityParser::parse(
    const quantra::VolatilityTermStructure *vol)
{
    if (vol == NULL)
        QUANTRA_ERROR("VolatilityTermStructure not found");

    // Parse reference date
    QuantLib::Date referenceDate = DateToQL(vol->reference_date()->str());

    // Parse calendar and conventions
    QuantLib::Calendar calendar = CalendarToQL(vol->calendar());
    QuantLib::BusinessDayConvention bdc = ConventionToQL(vol->business_day_convention());
    QuantLib::DayCounter dayCounter = DayCounterToQL(vol->day_counter());

    // Determine volatility type (use QuantLib:: prefix to avoid ambiguity)
    QuantLib::VolatilityType volType;
    switch (vol->volatility_type()) {
        case quantra::enums::VolatilityType_ShiftedLognormal:
            volType = QuantLib::ShiftedLognormal;
            break;
        case quantra::enums::VolatilityType_Normal:
            volType = QuantLib::Normal;
            break;
        default:
            volType = QuantLib::ShiftedLognormal;
    }

    // For now, support constant volatility
    double constantVol = vol->constant_vol();

    if (constantVol <= 0.0) {
        QUANTRA_ERROR("Constant volatility must be positive");
    }

    auto volStructure = std::make_shared<QuantLib::ConstantOptionletVolatility>(
        referenceDate,
        calendar,
        bdc,
        constantVol,
        dayCounter,
        volType
    );

    return volStructure;
}