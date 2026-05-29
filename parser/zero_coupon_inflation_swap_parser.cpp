#include "zero_coupon_inflation_swap_parser.h"

#include "common.h"
#include "common_parser.h"
#include "enums.h"
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

    QuantLib::ZeroCouponInflationSwap::Type swapType;
    switch (swap->swap_type()) {
        case quantra::enums::SwapType_Payer:
            swapType = QuantLib::ZeroCouponInflationSwap::Payer;
            break;
        case quantra::enums::SwapType_Receiver:
            swapType = QuantLib::ZeroCouponInflationSwap::Receiver;
            break;
        default:
            QUANTRA_INVALID_ARGUMENT("Invalid zero coupon inflation swap type");
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
