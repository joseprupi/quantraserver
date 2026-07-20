#include "zero_coupon_inflation_swap_parser.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"

namespace {

QuantLib::CPI::InterpolationType toQlInterpolation(quantra::enums::CPIInterpolationType interpolation) {
    switch (interpolation) {
        case quantra::enums::CPIInterpolationType_AsIndex:
            return QuantLib::CPI::AsIndex;
        case quantra::enums::CPIInterpolationType_Flat:
            return QuantLib::CPI::Flat;
        case quantra::enums::CPIInterpolationType_Linear:
            return QuantLib::CPI::Linear;
    }
    QUANTRA_INVALID_ARGUMENT("Unsupported CPI interpolation type");
    return QuantLib::CPI::AsIndex;
}

} // namespace

std::shared_ptr<QuantLib::ZeroCouponInflationSwap> ZeroCouponInflationSwapParser::parse(
    const quantra::ZeroCouponInflationSwap* swap,
    const std::shared_ptr<QuantLib::ZeroInflationIndex>& inflationIndex) {
    if (!swap) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponInflationSwap not found");
    }
    if (!swap->start_date()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponInflationSwap start_date not found");
    }
    if (!swap->maturity_date()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponInflationSwap maturity_date not found");
    }
    if (!swap->observation_lag()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponInflationSwap observation_lag not found");
    }
    if (!swap->inflation_index_id() || swap->inflation_index_id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponInflationSwap inflation_index_id is required");
    }
    if (!inflationIndex) {
        QUANTRA_NOT_FOUND("ZeroCouponInflationSwap inflation index not available");
    }
    if (swap->adjust_observation_dates()) {
        QUANTRA_INVALID_ARGUMENT(
            "ZeroCouponInflationSwap adjust_observation_dates=true is not supported: "
            "in the pinned QuantLib it does not change the priced observation dates "
            "for this instrument, so honouring it would be a silent no-op. Omit the "
            "field or set it to false.");
    }
    if (!swap->swap_type().has_value())
        QUANTRA_INVALID_ARGUMENT("ZeroCouponInflationSwap.swap_type is required");

    QuantLib::ZeroCouponInflationSwap::Type swapType;
    switch (swap->swap_type().value()) {
        case quantra::enums::SwapType_Payer:
            swapType = QuantLib::ZeroCouponInflationSwap::Payer;
            break;
        case quantra::enums::SwapType_Receiver:
            swapType = QuantLib::ZeroCouponInflationSwap::Receiver;
            break;
        default:
            QUANTRA_INVALID_ARGUMENT(
                "ZeroCouponInflationSwap.swap_type is not a known swap type: " +
                std::to_string(static_cast<int>(swap->swap_type().value())));
    }

    return std::make_shared<QuantLib::ZeroCouponInflationSwap>(
        swapType,
        swap->notional(),
        DateToQL(swap->start_date()->str()),
        DateToQL(swap->maturity_date()->str()),
        CalendarToQL(swap->fixed_calendar()),
        ConventionToQL(swap->fixed_convention()),
        DayCounterToQL(swap->day_counter()),
        swap->fixed_rate(),
        inflationIndex,
        QuantLib::Period(
            swap->observation_lag()->n(),
            TimeUnitToQL(swap->observation_lag()->unit())),
        toQlInterpolation(swap->observation_interpolation()),
        swap->adjust_observation_dates(),
        CalendarToQL(swap->inflation_calendar()),
        ConventionToQL(swap->inflation_convention()));
}
