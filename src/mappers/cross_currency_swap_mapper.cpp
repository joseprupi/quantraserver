#include "cross_currency_swap_mapper.h"

#include "enum_convert.h"
#include "error.h"
#include "fx_spot_parser.h" // RequireIsoCurrency
#include "request_validation.h"
#include "schedule_parser.h"

#include <cmath>

namespace quantra {

namespace {

std::string requireId(const flatbuffers::String* value, const std::string& field) {
    if (!value || value->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(field + " is required");
    }
    return value->str();
}

XccyLegData extractLeg(const quantra::XccyLeg* fb, const std::string& legName) {
    if (fb == nullptr) {
        QUANTRA_INVALID_ARGUMENT("CrossCurrencySwap." + legName + " is required");
    }
    const std::string prefix = "CrossCurrencySwap." + legName + ".";

    XccyLegData leg;
    leg.currency = RequireIsoCurrency(fb->currency(), prefix + "currency");

    const bool hasFixed = fb->fixed_leg() != nullptr;
    const bool hasFloating = fb->floating_leg() != nullptr;
    if (hasFixed == hasFloating) {
        QUANTRA_INVALID_ARGUMENT(
            "CrossCurrencySwap." + legName +
            " must contain exactly one of fixed_leg or floating_leg");
    }
    leg.initialExchange = requireBool(fb->initial_exchange(), prefix + "initial_exchange");
    leg.finalExchange = requireBool(fb->final_exchange(), prefix + "final_exchange");
    leg.discountingCurveId = requireId(fb->discounting_curve(), prefix + "discounting_curve");

    const bool hasForwarding = fb->forwarding_curve() && !fb->forwarding_curve()->str().empty();
    if (hasFloating && !hasForwarding) {
        QUANTRA_INVALID_ARGUMENT(prefix + "forwarding_curve is required with a floating_leg");
    }
    if (hasFixed && hasForwarding) {
        QUANTRA_INVALID_ARGUMENT(
            prefix +
            "forwarding_curve must be absent with a fixed_leg (a fixed leg projects nothing)");
    }

    ScheduleParser scheduleParser;
    if (hasFixed) {
        const auto* f = fb->fixed_leg();
        if (!f->schedule()) {
            QUANTRA_INVALID_ARGUMENT(prefix + "fixed_leg.schedule is required");
        }
        rejectUnsupportedNotionals(f->notionals(), "CrossCurrencySwap " + legName + " fixed leg");
        leg.kind = XccyLegData::Kind::Fixed;
        leg.fixed.schedule = *scheduleParser.parse(f->schedule());
        leg.notional = requirePositive(f->notional(), prefix + "fixed_leg.notional");
        leg.fixed.notionals = {leg.notional};
        leg.fixed.rate = f->rate();
        if (!std::isfinite(leg.fixed.rate)) {
            QUANTRA_INVALID_ARGUMENT(prefix + "fixed_leg.rate must be finite");
        }
        leg.fixed.dayCounter =
            DayCounterToQL(requireEnum(f->day_counter(), prefix + "fixed_leg.day_counter"));
        leg.fixed.paymentConvention = ConventionToQL(
            requireEnum(f->payment_convention(), prefix + "fixed_leg.payment_convention"));
        return leg;
    }

    const auto* f = fb->floating_leg();
    if (!f->schedule()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "floating_leg.schedule is required");
    }
    if (!f->index() || !f->index()->id() || f->index()->id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "floating_leg.index.id is required");
    }
    rejectUnsupportedNotionals(f->notionals(), "CrossCurrencySwap " + legName + " floating leg");
    leg.kind = XccyLegData::Kind::Floating;
    leg.forwardingCurveId = fb->forwarding_curve()->str();
    leg.indexId = f->index()->id()->str();
    leg.floating.schedule = *scheduleParser.parse(f->schedule());
    leg.notional = requirePositive(f->notional(), prefix + "floating_leg.notional");
    leg.floating.notionals = {leg.notional};
    leg.floating.spread = f->spread();
    if (!std::isfinite(leg.floating.spread)) {
        QUANTRA_INVALID_ARGUMENT(prefix + "floating_leg.spread must be finite");
    }
    leg.floating.dayCounter =
        DayCounterToQL(requireEnum(f->day_counter(), prefix + "floating_leg.day_counter"));
    leg.floating.paymentConvention = ConventionToQL(
        requireEnum(f->payment_convention(), prefix + "floating_leg.payment_convention"));
    leg.floating.fixingDays =
        requireNonNegative(f->fixing_days(), prefix + "floating_leg.fixing_days");
    leg.floating.inArrears = f->in_arrears();
    return leg;
}

CrossCurrencySwapTrade extractTrade(const quantra::PriceCrossCurrencySwap* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCrossCurrencySwap entry is null");
    }
    const auto* swap = pricing->cross_currency_swap();
    if (!swap) {
        QUANTRA_INVALID_ARGUMENT("PriceCrossCurrencySwap.cross_currency_swap is required");
    }

    CrossCurrencySwapTrade trade;
    trade.swapType = SwapTypeToQL(requireEnum(swap->swap_type(), "CrossCurrencySwap.swap_type"));
    trade.leg1 = extractLeg(swap->leg1(), "leg1");
    trade.leg2 = extractLeg(swap->leg2(), "leg2");
    if (trade.leg1.currency == trade.leg2.currency) {
        QUANTRA_INVALID_ARGUMENT(
            "CrossCurrencySwap.leg1.currency and CrossCurrencySwap.leg2.currency must differ, "
            "both are '" +
            trade.leg1.currency +
            "' (two legs in one currency are a basis or a vanilla swap: use "
            "price-basis-swap / price-vanilla-swap)");
    }
    trade.fxSpotId = requireId(pricing->fx_spot(), "PriceCrossCurrencySwap.fx_spot");
    trade.reportingCurrency = RequireIsoCurrency(
        pricing->reporting_currency(), "PriceCrossCurrencySwap.reporting_currency");
    if (trade.reportingCurrency != trade.leg1.currency &&
        trade.reportingCurrency != trade.leg2.currency) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCrossCurrencySwap.reporting_currency '" + trade.reportingCurrency + "' must be " +
            trade.leg1.currency + " or " + trade.leg2.currency +
            " (reporting in a third currency is not supported)");
    }
    return trade;
}

quantra::enums::SwapFlowKind toFbKind(SwapFlowKindDomain kind) {
    switch (kind) {
        case SwapFlowKindDomain::Coupon: return quantra::enums::SwapFlowKind_Coupon;
        case SwapFlowKindDomain::NotionalExchange:
            return quantra::enums::SwapFlowKind_NotionalExchange;
    }
    QUANTRA_ERROR("Swap flow kind not found");
    return quantra::enums::SwapFlowKind_Coupon;
}

flatbuffers::Offset<quantra::SwapLegFlow> serializeFlow(
    flatbuffers::grpc::MessageBuilder& builder, const SwapLegFlowPlain& f) {
    auto paymentDate = builder.CreateString(f.paymentDate);
    const bool coupon = f.kind == SwapFlowKindDomain::Coupon;
    flatbuffers::Offset<flatbuffers::String> accrualStart = 0, accrualEnd = 0, fixingDate = 0;
    if (coupon) {
        accrualStart = builder.CreateString(f.accrualStartDate);
        accrualEnd = builder.CreateString(f.accrualEndDate);
        if (f.isFloating) {
            fixingDate = builder.CreateString(f.fixingDate);
        }
    }

    quantra::SwapLegFlowBuilder fb(builder);
    fb.add_payment_date(paymentDate);
    fb.add_amount(f.amount);
    fb.add_discount(f.discount);
    fb.add_present_value(f.presentValue);
    fb.add_kind(toFbKind(f.kind));
    if (coupon) {
        fb.add_accrual_start_date(accrualStart);
        fb.add_accrual_end_date(accrualEnd);
        fb.add_accrual_year_fraction(f.accrualYearFraction);
        fb.add_gearing(f.gearing);
        fb.add_rate(f.rate);
        if (f.isFloating) {
            fb.add_fixing_date(fixingDate);
            if (std::isfinite(f.indexFixing)) fb.add_index_fixing(f.indexFixing);
            fb.add_spread(f.spread);
        }
    }
    return fb.Finish();
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegFlow>>> serializeFlows(
    flatbuffers::grpc::MessageBuilder& builder, const std::vector<SwapLegFlowPlain>& flows) {
    std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> offsets;
    offsets.reserve(flows.size());
    for (const auto& f : flows) {
        offsets.push_back(serializeFlow(builder, f));
    }
    return builder.CreateVector(offsets);
}

} // namespace

CrossCurrencySwapInputs CrossCurrencySwapMapper::toInputs(
    const quantra::PriceCrossCurrencySwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCrossCurrencySwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCrossCurrencySwapRequest.swaps is required and must be non-empty");
    }

    CrossCurrencySwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceCrossCurrencySwapResponse> CrossCurrencySwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const CrossCurrencySwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::CrossCurrencySwapResponse>> rows;
    rows.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegFlow>>>
            leg1Flows = 0,
            leg2Flows = 0;
        if (swap.includeFlows) {
            leg1Flows = serializeFlows(builder, swap.leg1Flows);
            leg2Flows = serializeFlows(builder, swap.leg2Flows);
        }
        auto spotDate = builder.CreateString(swap.spotDate);

        quantra::CrossCurrencySwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        rb.add_leg1_npv(swap.leg1Npv);
        rb.add_leg2_npv(swap.leg2Npv);
        rb.add_leg1_npv_reporting(swap.leg1NpvReporting);
        rb.add_leg2_npv_reporting(swap.leg2NpvReporting);
        // Optional by construction: present only when the swap shape defines
        // them (one fixed leg -> fair_rate; two floating legs -> fair_spread).
        if (swap.fairRate && std::isfinite(*swap.fairRate)) rb.add_fair_rate(*swap.fairRate);
        if (swap.fairSpread && std::isfinite(*swap.fairSpread)) {
            rb.add_fair_spread(*swap.fairSpread);
        }
        rb.add_fx_spot_used(swap.fxSpotUsed);
        rb.add_fx_as_of_rate(swap.fxAsOfRate);
        rb.add_spot_date(spotDate);
        if (swap.includeFlows) {
            rb.add_leg1_flows(leg1Flows);
            rb.add_leg2_flows(leg2Flows);
        }
        rows.push_back(rb.Finish());
    }

    auto rowsVec = builder.CreateVector(rows);
    quantra::PriceCrossCurrencySwapResponseBuilder rb(builder);
    rb.add_swaps(rowsVec);
    return rb.Finish();
}

} // namespace quantra
