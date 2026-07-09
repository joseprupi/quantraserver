#include "ois_swap_mapper.h"

#include "schedule_parser.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

namespace {

OisSwapTrade extractTrade(const quantra::PriceOisSwap* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwap entry is null");
    }
    if (!pricing->ois_swap()) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwap.ois_swap is required");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwap.discounting_curve is required");
    }
    if (!pricing->forwarding_curve() || pricing->forwarding_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwap.forwarding_curve is required");
    }

    const auto* swap = pricing->ois_swap();
    if (!swap->fixed_leg()) {
        QUANTRA_INVALID_ARGUMENT("OisSwap.fixed_leg is required");
    }
    if (!swap->overnight_leg()) {
        QUANTRA_INVALID_ARGUMENT("OisSwap.overnight_leg is required");
    }

    const auto* fixedFb = swap->fixed_leg();
    if (!fixedFb->schedule()) {
        QUANTRA_INVALID_ARGUMENT("OisSwap.fixed_leg.schedule is required");
    }
    if (!fixedFb->day_counter().has_value()) {
        QUANTRA_INVALID_ARGUMENT("SwapFixedLeg.day_counter is required");
    }

    const auto* overnightFb = swap->overnight_leg();
    if (!overnightFb->schedule()) {
        QUANTRA_INVALID_ARGUMENT("OisSwap.overnight_leg.schedule is required");
    }
    if (!overnightFb->index() || !overnightFb->index()->id()) {
        QUANTRA_INVALID_ARGUMENT("OisSwap.overnight_leg.index.id is required");
    }

    ScheduleParser scheduleParser;

    OisSwapTrade trade;
    trade.swapType = OvernightIndexedSwapTypeToQL(swap->swap_type());
    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.forwardingCurveId = pricing->forwarding_curve()->str();

    auto fixedSchedule = scheduleParser.parse(fixedFb->schedule());
    trade.fixed.schedule = *fixedSchedule;
    trade.fixed.notional = fixedFb->notional();
    trade.fixed.rate = fixedFb->rate();
    trade.fixed.dayCounter = DayCounterToQL(fixedFb->day_counter().value());

    auto overnightSchedule = scheduleParser.parse(overnightFb->schedule());
    trade.overnight.schedule = *overnightSchedule;
    trade.overnight.notional = overnightFb->notional();
    trade.overnight.indexId = overnightFb->index()->id()->str();
    trade.overnight.spread = overnightFb->spread();
    trade.overnight.paymentConvention = ConventionToQL(overnightFb->payment_convention());
    trade.overnight.paymentCalendar = CalendarToQL(overnightFb->payment_calendar());
    trade.overnight.paymentLag = overnightFb->payment_lag();
    trade.overnight.averagingMethod = RateAveragingToQL(overnightFb->averaging_method());
    trade.overnight.lookbackDays = overnightFb->lookback_days();
    trade.overnight.lockoutDays = overnightFb->lockout_days();
    trade.overnight.applyObservationShift = overnightFb->apply_observation_shift();
    trade.overnight.telescopicValueDates = overnightFb->telescopic_value_dates();
    return trade;
}

flatbuffers::Offset<quantra::SwapLegFlow> serializeFlow(
    flatbuffers::grpc::MessageBuilder& builder,
    const OisSwapFlowPlain& f) {
    auto paymentDate = builder.CreateString(f.paymentDate);
    auto accrualStart = builder.CreateString(f.accrualStartDate);
    auto accrualEnd = builder.CreateString(f.accrualEndDate);
    flatbuffers::Offset<flatbuffers::String> fixingDate = 0;
    if (f.isFloating) {
        fixingDate = builder.CreateString(f.fixingDate);
    }

    quantra::SwapLegFlowBuilder fb(builder);
    fb.add_payment_date(paymentDate);
    fb.add_accrual_start_date(accrualStart);
    fb.add_accrual_end_date(accrualEnd);
    fb.add_accrual_year_fraction(f.accrualYearFraction);
    fb.add_amount(f.amount);
    fb.add_discount(f.discount);
    fb.add_present_value(f.presentValue);
    fb.add_rate(f.rate);
    if (f.isFloating) {
        fb.add_fixing_date(fixingDate);
        fb.add_index_fixing(f.indexFixing);
        fb.add_spread(f.spread);
    }
    return fb.Finish();
}

} // namespace

OisSwapInputs OisSwapMapper::toInputs(const quantra::PriceOisSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceOisSwapRequest.swaps is required and must be non-empty");
    }

    OisSwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceOisSwapResponse> OisSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const OisSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::OisSwapResponse>> swapsVector;
    swapsVector.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> fixedFlowOffsets;
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> overnightFlowOffsets;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegFlow>>>
            fixedFlows = 0;
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegFlow>>>
            overnightFlows = 0;
        if (swap.includeFlows) {
            fixedFlowOffsets.reserve(swap.fixedFlows.size());
            for (const auto& f : swap.fixedFlows) {
                fixedFlowOffsets.push_back(serializeFlow(builder, f));
            }
            overnightFlowOffsets.reserve(swap.overnightFlows.size());
            for (const auto& f : swap.overnightFlows) {
                overnightFlowOffsets.push_back(serializeFlow(builder, f));
            }
            fixedFlows = builder.CreateVector(fixedFlowOffsets);
            overnightFlows = builder.CreateVector(overnightFlowOffsets);
        }

        quantra::OisSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        rb.add_fair_rate(swap.fairRate);
        rb.add_fair_spread(swap.fairSpread);
        rb.add_fixed_leg_bps(swap.fixedLegBps);
        rb.add_overnight_leg_bps(swap.overnightLegBps);
        rb.add_fixed_leg_npv(swap.fixedLegNpv);
        rb.add_overnight_leg_npv(swap.overnightLegNpv);
        if (swap.includeFlows) {
            rb.add_fixed_leg_flows(fixedFlows);
            rb.add_overnight_leg_flows(overnightFlows);
        }
        swapsVector.push_back(rb.Finish());
    }

    auto swapsVec = builder.CreateVector(swapsVector);
    quantra::PriceOisSwapResponseBuilder rb(builder);
    rb.add_swaps(swapsVec);
    return rb.Finish();
}

} // namespace quantra
