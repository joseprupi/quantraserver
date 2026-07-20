#include "zero_coupon_inflation_swap_mapper.h"

#include <cmath>

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

namespace {

ZeroCouponInflationSwapTrade extractTrade(const quantra::PriceZeroCouponInflationSwap* pricing) {
    if (!pricing) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponInflationSwap entry is null");
    }
    if (!pricing->zero_coupon_inflation_swap()) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceZeroCouponInflationSwap entry requires zero_coupon_inflation_swap");
    }
    if (!pricing->discounting_curve()) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceZeroCouponInflationSwap entry requires discounting_curve");
    }
    if (!pricing->inflation_curve()) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceZeroCouponInflationSwap entry requires inflation_curve");
    }

    const auto* swap = pricing->zero_coupon_inflation_swap();
    if (!swap->swap_type().has_value()) {
        QUANTRA_INVALID_ARGUMENT("ZeroCouponInflationSwap.swap_type is required");
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
        QUANTRA_INVALID_ARGUMENT(
            "ZeroCouponInflationSwap inflation_index_id is required");
    }

    ZeroCouponInflationSwapTrade trade;
    trade.swapType = ZeroCouponInflationSwapTypeToQL(swap->swap_type().value());
    trade.notional = swap->notional();
    trade.startDate = DateToQL(swap->start_date()->str());
    trade.maturityDate = DateToQL(swap->maturity_date()->str());
    trade.fixedCalendar = CalendarToQL(swap->fixed_calendar());
    trade.fixedConvention = ConventionToQL(swap->fixed_convention());
    trade.dayCounter = DayCounterToQL(swap->day_counter());
    trade.fixedRate = swap->fixed_rate();
    trade.inflationIndexId = swap->inflation_index_id()->str();
    trade.observationLag = QuantLib::Period(
        swap->observation_lag()->n(),
        TimeUnitToQL(swap->observation_lag()->unit()));
    trade.observationInterpolation = CPIInterpolationToQL(swap->observation_interpolation());
    if (swap->adjust_observation_dates()) {
        QUANTRA_INVALID_ARGUMENT(
            "ZeroCouponInflationSwap adjust_observation_dates=true is not supported: "
            "in the pinned QuantLib it does not change the priced observation dates "
            "for this instrument, so honouring it would be a silent no-op. Omit the "
            "field or set it to false.");
    }
    trade.adjustObservationDates = swap->adjust_observation_dates();
    trade.inflationCalendar = CalendarToQL(swap->inflation_calendar());
    trade.inflationConvention = ConventionToQL(swap->inflation_convention());
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
    const ZeroCouponInflationSwapFlowPlain& f) {
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

ZeroCouponInflationSwapInputs ZeroCouponInflationSwapMapper::toInputs(
    const quantra::PriceZeroCouponInflationSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceZeroCouponInflationSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceZeroCouponInflationSwapRequest.swaps is required and must be non-empty");
    }

    ZeroCouponInflationSwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceZeroCouponInflationSwapResponse>
ZeroCouponInflationSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const ZeroCouponInflationSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::ZeroCouponInflationSwapResponse>> swapsVector;
    swapsVector.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> fixedFlowOffsets;
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> inflationFlowOffsets;
        if (swap.includeFlows) {
            fixedFlowOffsets.reserve(swap.fixedFlows.size());
            for (const auto& f : swap.fixedFlows) {
                fixedFlowOffsets.push_back(serializeFlow(builder, f));
            }
            inflationFlowOffsets.reserve(swap.inflationFlows.size());
            for (const auto& f : swap.inflationFlows) {
                inflationFlowOffsets.push_back(serializeFlow(builder, f));
            }
        }
        auto fixedFlows = builder.CreateVector(fixedFlowOffsets);
        auto inflationFlows = builder.CreateVector(inflationFlowOffsets);

        quantra::ZeroCouponInflationSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        // fairRate divides by leg BPS; a zero-BPS leg (e.g. notional 0) yields
        // NaN/inf, which is not valid JSON. Omit rather than emit `nan`.
        if (std::isfinite(swap.fairRate)) rb.add_fair_rate(swap.fairRate);
        rb.add_fixed_leg_bps(swap.fixedLegBps);
        rb.add_fixed_leg_npv(swap.fixedLegNpv);
        rb.add_inflation_leg_npv(swap.inflationLegNpv);
        if (swap.includeFlows) {
            rb.add_fixed_leg_flows(fixedFlows);
            rb.add_inflation_leg_flows(inflationFlows);
        }
        swapsVector.push_back(rb.Finish());
    }

    auto swapsVec = builder.CreateVector(swapsVector);
    quantra::PriceZeroCouponInflationSwapResponseBuilder rb(builder);
    rb.add_swaps(swapsVec);
    return rb.Finish();
}

} // namespace quantra
