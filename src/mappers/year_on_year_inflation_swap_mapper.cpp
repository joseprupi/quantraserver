#include "year_on_year_inflation_swap_mapper.h"

#include <cmath>

#include "date_convert.h"
#include "schedule_parser.h"
#include "enum_convert.h"
#include "error.h"
#include "require_scalar.h"
#include "require_period.h"

namespace quantra {

namespace {

YearOnYearInflationSwapTrade extractTrade(const quantra::PriceYearOnYearInflationSwap* pricing) {
    if (!pricing) {
        QUANTRA_INVALID_ARGUMENT("PriceYearOnYearInflationSwap entry is null");
    }
    if (!pricing->year_on_year_inflation_swap()) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceYearOnYearInflationSwap entry requires year_on_year_inflation_swap");
    }
    if (!pricing->discounting_curve()) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceYearOnYearInflationSwap entry requires discounting_curve");
    }
    if (!pricing->inflation_curve()) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceYearOnYearInflationSwap entry requires inflation_curve");
    }

    const auto* swap = pricing->year_on_year_inflation_swap();
    if (!swap->swap_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap.swap_type is required");
    }
    if (!swap->fixed_schedule()) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap fixed_schedule not found");
    }
    if (!swap->yoy_schedule()) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap yoy_schedule not found");
    }
    if (!swap->observation_lag()) {
        QUANTRA_INVALID_ARGUMENT("YearOnYearInflationSwap observation_lag not found");
    }
    if (!swap->inflation_index_id() || swap->inflation_index_id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(
            "YearOnYearInflationSwap inflation_index_id is required");
    }

    ScheduleParser scheduleParser;
    auto fixedSchedule = scheduleParser.parse(swap->fixed_schedule());
    auto yoySchedule = scheduleParser.parse(swap->yoy_schedule());

    YearOnYearInflationSwapTrade trade;
    trade.swapType = YearOnYearInflationSwapTypeToQL(swap->swap_type().value());
    trade.notional = requirePositive(swap->notional(), "YearOnYearInflationSwap.notional");
    trade.fixedSchedule = *fixedSchedule;
    trade.fixedRate = swap->fixed_rate();
    trade.fixedDayCounter = DayCounterToQL(swap->fixed_day_counter());
    trade.yoySchedule = *yoySchedule;
    trade.inflationIndexId = swap->inflation_index_id()->str();
    trade.observationLag = requirePeriod(
        swap->observation_lag(), "YearOnYearInflationSwap.observation_lag");
    trade.observationInterpolation = CPIInterpolationToQL(swap->observation_interpolation());
    trade.spread = swap->spread();
    trade.yoyDayCounter = DayCounterToQL(swap->yoy_day_counter());
    trade.paymentCalendar = CalendarToQL(swap->payment_calendar());
    trade.paymentConvention = ConventionToQL(swap->payment_convention());
    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.inflationCurveId = pricing->inflation_curve()->str();
    return trade;
}

/**
 * Serialize one plain flow into a SwapLegFlow offset. Mirrors
 * parser/swap_leg_flow_builder.cpp exactly: accrual fields are added only
 * when the underlying cashflow was a Coupon; fixing fields only when it was
 * a FloatingRateCoupon. amount/discount/present_value/rate are always
 * written (rate defaults to 0.0 for plain CashFlows, matching the legacy
 * default value the FB schema bakes in).
 */
flatbuffers::Offset<quantra::SwapLegFlow> serializeFlow(
    flatbuffers::grpc::MessageBuilder& builder,
    const YearOnYearInflationSwapFlowPlain& f) {
    auto paymentDate = builder.CreateString(f.paymentDate);
    flatbuffers::Offset<flatbuffers::String> accrualStart = 0;
    flatbuffers::Offset<flatbuffers::String> accrualEnd = 0;
    if (f.isCoupon) {
        accrualStart = builder.CreateString(f.accrualStartDate);
        accrualEnd = builder.CreateString(f.accrualEndDate);
    }
    flatbuffers::Offset<flatbuffers::String> fixingDate = 0;
    if (f.isFloating) {
        fixingDate = builder.CreateString(f.fixingDate);
    }

    quantra::SwapLegFlowBuilder fb(builder);
    fb.add_payment_date(paymentDate);
    if (f.isCoupon) {
        fb.add_accrual_start_date(accrualStart);
        fb.add_accrual_end_date(accrualEnd);
        fb.add_accrual_year_fraction(f.accrualYearFraction);
    }
    fb.add_amount(f.amount);
    fb.add_discount(f.discount);
    fb.add_present_value(f.presentValue);
    fb.add_rate(f.rate);
    if (f.isFloating) {
        fb.add_fixing_date(fixingDate);
        if (std::isfinite(f.indexFixing)) fb.add_index_fixing(f.indexFixing);
        fb.add_spread(f.spread);
    }
    return fb.Finish();
}

} // namespace

YearOnYearInflationSwapInputs YearOnYearInflationSwapMapper::toInputs(
    const quantra::PriceYearOnYearInflationSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceYearOnYearInflationSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceYearOnYearInflationSwapRequest.swaps is required and must be non-empty");
    }

    YearOnYearInflationSwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceYearOnYearInflationSwapResponse>
YearOnYearInflationSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const YearOnYearInflationSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::YearOnYearInflationSwapResponse>> swapsVector;
    swapsVector.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> fixedFlowOffsets;
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> yoyFlowOffsets;
        if (swap.includeFlows) {
            fixedFlowOffsets.reserve(swap.fixedFlows.size());
            for (const auto& f : swap.fixedFlows) {
                fixedFlowOffsets.push_back(serializeFlow(builder, f));
            }
            yoyFlowOffsets.reserve(swap.yoyFlows.size());
            for (const auto& f : swap.yoyFlows) {
                yoyFlowOffsets.push_back(serializeFlow(builder, f));
            }
        }
        auto fixedFlows = builder.CreateVector(fixedFlowOffsets);
        auto yoyFlows = builder.CreateVector(yoyFlowOffsets);

        quantra::YearOnYearInflationSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        // fairRate/fairSpread divide by leg BPS; a zero-BPS leg (e.g. notional 0)
        // yields NaN/inf, which is not valid JSON. Omit rather than emit `nan`.
        if (std::isfinite(swap.fairRate)) rb.add_fair_rate(swap.fairRate);
        if (std::isfinite(swap.fairSpread)) rb.add_fair_spread(swap.fairSpread);
        rb.add_fixed_leg_bps(swap.fixedLegBps);
        rb.add_yoy_leg_bps(swap.yoyLegBps);
        rb.add_fixed_leg_npv(swap.fixedLegNpv);
        rb.add_yoy_leg_npv(swap.yoyLegNpv);
        if (swap.includeFlows) {
            rb.add_fixed_leg_flows(fixedFlows);
            rb.add_yoy_leg_flows(yoyFlows);
        }
        swapsVector.push_back(rb.Finish());
    }

    auto swapsVec = builder.CreateVector(swapsVector);
    quantra::PriceYearOnYearInflationSwapResponseBuilder rb(builder);
    rb.add_swaps(swapsVec);
    return rb.Finish();
}

} // namespace quantra
