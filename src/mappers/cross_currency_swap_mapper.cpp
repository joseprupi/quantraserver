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

double requireFiniteLevel(double value, const std::string& field) {
    if (!std::isfinite(value)) {
        QUANTRA_INVALID_ARGUMENT(field + " must be finite");
    }
    return value;
}

void extractFixed(
    const quantra::FixedCoupons* f,
    const std::string& prefix,
    ScheduleParser& scheduleParser,
    SwapLegData& leg) {
    if (!f->schedule()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "schedule is required");
    }
    rejectUnsupportedNotionals(f->notionals(), prefix + "notionals (price-swap)");
    leg.kind = SwapLegData::Kind::Fixed;
    leg.fixed.schedule = *scheduleParser.parse(f->schedule());
    leg.notional = requirePositive(f->notional(), prefix + "notional");
    leg.fixed.notionals = {leg.notional};
    leg.fixed.rate = requireFiniteLevel(f->rate(), prefix + "rate");
    leg.fixed.dayCounter = DayCounterToQL(requireEnum(f->day_counter(), prefix + "day_counter"));
    leg.fixed.paymentConvention =
        ConventionToQL(requireEnum(f->payment_convention(), prefix + "payment_convention"));
}

void extractIbor(
    const quantra::IborCoupons* f,
    const std::string& prefix,
    ScheduleParser& scheduleParser,
    SwapLegData& leg) {
    if (!f->schedule()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "schedule is required");
    }
    if (!f->index() || !f->index()->id() || f->index()->id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "index.id is required");
    }
    rejectUnsupportedNotionals(f->notionals(), prefix + "notionals (price-swap)");
    leg.kind = SwapLegData::Kind::Ibor;
    leg.indexId = f->index()->id()->str();
    leg.ibor.schedule = *scheduleParser.parse(f->schedule());
    leg.notional = requirePositive(f->notional(), prefix + "notional");
    leg.ibor.notionals = {leg.notional};
    leg.ibor.spread = requireFiniteLevel(f->spread(), prefix + "spread");
    leg.ibor.dayCounter = DayCounterToQL(requireEnum(f->day_counter(), prefix + "day_counter"));
    leg.ibor.paymentConvention =
        ConventionToQL(requireEnum(f->payment_convention(), prefix + "payment_convention"));
    leg.ibor.fixingDays = requireNonNegative(f->fixing_days(), prefix + "fixing_days");
    leg.ibor.inArrears = f->in_arrears();
}

void extractOvernight(
    const quantra::OvernightCoupons* f,
    const std::string& prefix,
    ScheduleParser& scheduleParser,
    SwapLegData& leg) {
    if (!f->schedule()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "schedule is required");
    }
    if (!f->index() || !f->index()->id() || f->index()->id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "index.id is required");
    }
    leg.kind = SwapLegData::Kind::Overnight;
    leg.indexId = f->index()->id()->str();
    leg.overnight.schedule = *scheduleParser.parse(f->schedule());
    leg.notional = requirePositive(f->notional(), prefix + "notional");
    leg.overnight.notionals = {leg.notional};
    leg.overnight.spread = requireFiniteLevel(f->spread(), prefix + "spread");
    // OvernightCoupons.day_counter is accepted-but-unused: the coupons take
    // the overnight index's day counter (as price-ois-swap does).
    leg.overnight.paymentConvention =
        ConventionToQL(requireEnum(f->payment_convention(), prefix + "payment_convention"));
    leg.overnight.paymentCalendar =
        CalendarToQL(requireEnum(f->payment_calendar(), prefix + "payment_calendar"));
    leg.overnight.paymentLag = requireNonNegativeInt(f->payment_lag(), prefix + "payment_lag");
    leg.overnight.averagingMethod =
        RateAveragingToQL(requireEnum(f->averaging_method(), prefix + "averaging_method"));
    leg.overnight.lookbackDays =
        requireNonNegativeInt(f->lookback_days(), prefix + "lookback_days");
    leg.overnight.lockoutDays = requireNonNegativeInt(f->lockout_days(), prefix + "lockout_days");
    leg.overnight.applyObservationShift =
        requireBool(f->apply_observation_shift(), prefix + "apply_observation_shift");
    leg.overnight.telescopicValueDates = f->telescopic_value_dates();
}

SwapLegData extractLeg(const quantra::SwapLeg* fb, const std::string& name) {
    if (fb == nullptr) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    SwapLegData leg;
    if (!fb->pay_receive().has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + ".pay_receive is required (Pay or Receive)");
    }
    leg.side = fb->pay_receive().value() == quantra::enums::PayReceive_Pay
                   ? SwapLegData::Side::Pay
                   : SwapLegData::Side::Receive;
    leg.currency = RequireIsoCurrency(fb->currency(), name + ".currency");

    const quantra::SwapCoupons kind = fb->coupons_type();
    const char* kindName = quantra::EnumNameSwapCoupons(kind);
    const bool floating =
        kind == quantra::SwapCoupons_IborCoupons || kind == quantra::SwapCoupons_OvernightCoupons;
    switch (kind) {
        case quantra::SwapCoupons_NONE:
            QUANTRA_INVALID_ARGUMENT(
                name + ".coupons is required (coupons_type FixedCoupons, IborCoupons or "
                       "OvernightCoupons with the matching coupons table)");
        case quantra::SwapCoupons_CmsCoupons:
            QUANTRA_INVALID_ARGUMENT(
                name + ".coupons: CmsCoupons is not supported on price-cross-currency-swap in "
                       "this version");
        default: break;
    }
    if (fb->coupons() == nullptr) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".coupons is required with coupons_type " + std::string(kindName));
    }

    leg.initialExchange = requireBool(fb->initial_exchange(), name + ".initial_exchange");
    leg.finalExchange = requireBool(fb->final_exchange(), name + ".final_exchange");
    leg.discountingCurveId = requireId(fb->discounting_curve(), name + ".discounting_curve");

    const bool hasForwarding = fb->forwarding_curve() && !fb->forwarding_curve()->str().empty();
    if (floating && !hasForwarding) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".forwarding_curve is required with " + std::string(kindName));
    }
    if (!floating && hasForwarding) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".forwarding_curve must be absent with FixedCoupons (a fixed leg projects "
                   "nothing)");
    }
    if (hasForwarding) {
        leg.forwardingCurveId = fb->forwarding_curve()->str();
    }

    ScheduleParser scheduleParser;
    const std::string prefix = name + ".coupons.";
    switch (kind) {
        case quantra::SwapCoupons_FixedCoupons:
            extractFixed(fb->coupons_as_FixedCoupons(), prefix, scheduleParser, leg);
            break;
        case quantra::SwapCoupons_IborCoupons:
            extractIbor(fb->coupons_as_IborCoupons(), prefix, scheduleParser, leg);
            break;
        case quantra::SwapCoupons_OvernightCoupons:
            extractOvernight(fb->coupons_as_OvernightCoupons(), prefix, scheduleParser, leg);
            break;
        default: QUANTRA_INVALID_ARGUMENT(name + ".coupons_type is not supported");
    }
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
    trade.legs.reserve(2);
    trade.legs.push_back(extractLeg(swap->leg1(), "CrossCurrencySwap.leg1"));
    trade.legs.push_back(extractLeg(swap->leg2(), "CrossCurrencySwap.leg2"));
    const SwapLegData& l0 = trade.legs[0];
    const SwapLegData& l1 = trade.legs[1];
    if (l0.side == l1.side) {
        QUANTRA_INVALID_ARGUMENT(
            std::string(
                "CrossCurrencySwap.leg1.pay_receive and CrossCurrencySwap.leg2.pay_receive "
                "are both ") +
            (l0.side == SwapLegData::Side::Pay ? "Pay" : "Receive") +
            ": one leg must be paid and the other received");
    }
    if (l0.currency == l1.currency) {
        QUANTRA_INVALID_ARGUMENT(
            "CrossCurrencySwap.leg1.currency and CrossCurrencySwap.leg2.currency must differ, "
            "both are '" +
            l0.currency +
            "' (two legs in one currency are a vanilla, OIS or basis swap: use "
            "price-vanilla-swap / price-ois-swap / price-basis-swap)");
    }
    trade.fxSpotId = requireId(pricing->fx_spot(), "PriceCrossCurrencySwap.fx_spot");
    trade.reportingCurrency = RequireIsoCurrency(
        pricing->reporting_currency(), "PriceCrossCurrencySwap.reporting_currency");
    if (trade.reportingCurrency != l0.currency && trade.reportingCurrency != l1.currency) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCrossCurrencySwap.reporting_currency '" + trade.reportingCurrency + "' must be " +
            l0.currency + " or " + l1.currency +
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

flatbuffers::Offset<quantra::SwapLegResult> serializeLeg(
    flatbuffers::grpc::MessageBuilder& builder, const SwapLegPerLeg& leg, bool includeFlows) {
    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegFlow>>> flows = 0;
    if (includeFlows) {
        std::vector<flatbuffers::Offset<quantra::SwapLegFlow>> offsets;
        offsets.reserve(leg.flows.size());
        for (const auto& f : leg.flows) {
            offsets.push_back(serializeFlow(builder, f));
        }
        flows = builder.CreateVector(offsets);
    }
    quantra::SwapLegResultBuilder lb(builder);
    lb.add_npv(leg.npv);
    lb.add_npv_reporting(leg.npvReporting);
    if (includeFlows) lb.add_flows(flows);
    return lb.Finish();
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
        std::vector<flatbuffers::Offset<quantra::SwapLegResult>> legOffsets;
        legOffsets.reserve(swap.legs.size());
        for (const auto& leg : swap.legs) {
            legOffsets.push_back(serializeLeg(builder, leg, swap.includeFlows));
        }
        auto legs = builder.CreateVector(legOffsets);
        auto spotDate = builder.CreateString(swap.spotDate);

        quantra::CrossCurrencySwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        rb.add_legs(legs);
        // Optional by construction: present only when the swap shape defines
        // them (one fixed leg -> fair_rate; no fixed leg -> fair_spread).
        if (swap.fairRate && std::isfinite(*swap.fairRate)) rb.add_fair_rate(*swap.fairRate);
        if (swap.fairSpread && std::isfinite(*swap.fairSpread)) {
            rb.add_fair_spread(*swap.fairSpread);
        }
        rb.add_fx_spot_used(swap.fxSpotUsed);
        rb.add_fx_as_of_rate(swap.fxAsOfRate);
        rb.add_spot_date(spotDate);
        rows.push_back(rb.Finish());
    }

    auto rowsVec = builder.CreateVector(rows);
    quantra::PriceCrossCurrencySwapResponseBuilder rb(builder);
    rb.add_swaps(rowsVec);
    return rb.Finish();
}

} // namespace quantra
