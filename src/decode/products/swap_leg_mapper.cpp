#include "swap_leg_mapper.h"

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

/// Constant notional, or the per-period vector when the role accepts one.
/// Returns the vector the leg builders take (one entry = constant).
std::vector<double> decodeNotionals(
    const quantra::SwapLeg* fb,
    std::size_t periods,
    const std::string& name,
    const SwapLegRules& rules,
    double& constant) {
    std::vector<double> out;
    if (!rules.notionalsAllowed) {
        rejectUnsupportedNotionals(fb->notionals(), name + ".notionals (" + rules.endpoint + ")");
    } else if (parseOptionalNotionals(fb->notionals(), periods, name, out)) {
        // The amortizing vector overrides the scalar; the scalar is then
        // optional. The first period's amount is the exchanged notional.
        constant = fb->notional().has_value() ? fb->notional().value() : out.front();
        if (!std::isfinite(constant) || !(constant > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(name + ".notional must be positive");
        }
        return out;
    }
    constant = requirePositive(fb->notional(), name + ".notional");
    return {constant};
}

/// The fields every coupon kind shares, read off the leg: schedule, notional
/// (or notionals), day count, payment conventions. `dayCounterRead` is false
/// for overnight coupons, whose day counter is the index's own.
struct SharedLegFields {
    QuantLib::Schedule schedule;
    std::vector<double> notionals;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::ModifiedFollowing;
    QuantLib::Calendar paymentCalendar;
    int paymentLag = 0;
};

SharedLegFields decodeShared(
    const quantra::SwapLeg* fb,
    const std::string& name,
    const SwapLegRules& rules,
    SwapLegData::Kind kind,
    SwapLegData& leg) {
    SharedLegFields shared;
    if (!fb->schedule()) {
        QUANTRA_INVALID_ARGUMENT(name + ".schedule is required");
    }
    ScheduleParser scheduleParser;
    shared.schedule = *scheduleParser.parse(fb->schedule());
    shared.notionals = decodeNotionals(fb, shared.schedule.size() - 1, name, rules, leg.notional);
    if (kind != SwapLegData::Kind::Overnight) {
        shared.dayCounter = DayCounterToQL(requireEnum(fb->day_counter(), name + ".day_counter"));
    }
    // (an overnight leg's day_counter is accepted but not read: the coupons
    // take the overnight index's own, as QuantLib's OvernightIndexedSwap does)
    shared.paymentConvention =
        ConventionToQL(requireEnum(fb->payment_convention(), name + ".payment_convention"));

    const bool hasCalendar = fb->payment_calendar().has_value();
    const bool hasLag = fb->payment_lag().has_value();
    if (kind == SwapLegData::Kind::Cms) {
        if (hasCalendar || hasLag) {
            QUANTRA_INVALID_ARGUMENT(
                name + (hasCalendar ? ".payment_calendar" : ".payment_lag") +
                " is not supported with CmsCoupons in this version (a CMS leg pays on its "
                "schedule dates)");
        }
    } else if (rules.paymentLagRequired) {
        shared.paymentCalendar =
            CalendarToQL(requireEnum(fb->payment_calendar(), name + ".payment_calendar"));
        shared.paymentLag = requireNonNegativeInt(fb->payment_lag(), name + ".payment_lag");
    } else {
        // Absent = the schedule's calendar / no lag (QuantLib's own defaults).
        if (hasCalendar) shared.paymentCalendar = CalendarToQL(fb->payment_calendar().value());
        if (hasLag)
            shared.paymentLag = requireNonNegativeInt(fb->payment_lag(), name + ".payment_lag");
    }
    return shared;
}

void decodeFixed(
    const quantra::FixedCoupons* f,
    const std::string& prefix,
    const SharedLegFields& shared,
    SwapLegData& leg) {
    leg.kind = SwapLegData::Kind::Fixed;
    leg.fixed.schedule = shared.schedule;
    leg.fixed.notionals = shared.notionals;
    leg.fixed.rate = requireFiniteLevel(f->rate(), prefix + "rate");
    leg.fixed.dayCounter = shared.dayCounter;
    leg.fixed.paymentConvention = shared.paymentConvention;
    leg.fixed.paymentCalendar = shared.paymentCalendar;
    leg.fixed.paymentLag = shared.paymentLag;
}

void decodeIbor(
    const quantra::IborCoupons* f,
    const std::string& prefix,
    const SharedLegFields& shared,
    SwapLegData& leg) {
    if (!f->index() || !f->index()->id() || f->index()->id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "index.id is required");
    }
    leg.kind = SwapLegData::Kind::Ibor;
    leg.indexId = f->index()->id()->str();
    leg.ibor.schedule = shared.schedule;
    leg.ibor.notionals = shared.notionals;
    leg.ibor.spread = requireFiniteLevel(f->spread(), prefix + "spread");
    leg.ibor.dayCounter = shared.dayCounter;
    leg.ibor.paymentConvention = shared.paymentConvention;
    leg.ibor.paymentCalendar = shared.paymentCalendar;
    leg.ibor.paymentLag = shared.paymentLag;
    // Absent = the index's own fixing days.
    if (f->fixing_days().has_value()) {
        leg.ibor.fixingDays = requireNonNegativeInt(f->fixing_days(), prefix + "fixing_days");
    }
    leg.ibor.inArrears = f->in_arrears();
}

void decodeOvernight(
    const quantra::OvernightCoupons* f,
    const std::string& prefix,
    const SharedLegFields& shared,
    SwapLegData& leg) {
    if (!f->index() || !f->index()->id() || f->index()->id()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "index.id is required");
    }
    leg.kind = SwapLegData::Kind::Overnight;
    leg.indexId = f->index()->id()->str();
    leg.overnight.schedule = shared.schedule;
    leg.overnight.notionals = shared.notionals;
    leg.overnight.spread = requireFiniteLevel(f->spread(), prefix + "spread");
    leg.overnight.paymentConvention = shared.paymentConvention;
    leg.overnight.paymentCalendar = shared.paymentCalendar;
    leg.overnight.paymentLag = shared.paymentLag;
    leg.overnight.averagingMethod =
        RateAveragingToQL(requireEnum(f->averaging_method(), prefix + "averaging_method"));
    leg.overnight.lookbackDays =
        requireNonNegativeInt(f->lookback_days(), prefix + "lookback_days");
    leg.overnight.lockoutDays = requireNonNegativeInt(f->lockout_days(), prefix + "lockout_days");
    leg.overnight.applyObservationShift =
        requireBool(f->apply_observation_shift(), prefix + "apply_observation_shift");
    leg.overnight.telescopicValueDates = f->telescopic_value_dates();
}

CmsPricerParams decodeCmsPricer(const quantra::CmsPricerSpec* ps) {
    CmsPricerParams params;
    if (!ps) {
        return params;
    }
    params.pricerType = ps->pricer_type();
    params.yieldCurveModel = ps->yield_curve_model();
    params.meanReversion = ps->mean_reversion();
    params.haganLowerLimit = ps->hagan_lower_limit();
    params.haganUpperLimit = ps->hagan_upper_limit();
    params.haganPrecision = ps->hagan_precision();
    params.haganHardUpperLimit = ps->hagan_hard_upper_limit();
    return params;
}

void decodeCms(
    const quantra::CmsCoupons* f,
    const std::string& name,
    const SharedLegFields& shared,
    SwapLegData& leg) {
    const std::string prefix = name + ".coupons.";
    leg.kind = SwapLegData::Kind::Cms;
    leg.cms.schedule = shared.schedule;
    // CmsLeg takes one notional (the role allowing per-period notionals is
    // the vanilla swap, whose CMS leg keeps the constant one).
    if (shared.notionals.size() != 1) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".notionals is not supported with CmsCoupons in this version");
    }
    leg.cms.notional = leg.notional;
    leg.cms.swapIndexId = requireId(f->swap_index_id(), prefix + "swap_index_id");
    leg.cms.swapTenor = requirePeriod(f->swap_tenor(), prefix + "swap_tenor");
    leg.cms.swaptionVolId = requireId(f->swaption_vol_id(), prefix + "swaption_vol_id");
    leg.cms.fixingDays = f->fixing_days();
    leg.cms.dayCounter = shared.dayCounter;
    leg.cms.paymentConvention = shared.paymentConvention;
    leg.cms.gear = requireFiniteLevel(f->gear(), prefix + "gear");
    leg.cms.spread = requireFiniteLevel(f->spread(), prefix + "spread");
    // Optional cap / floor strikes: presence (not sign) decides, a floor of 0
    // is a legitimate value.
    if (f->cap().has_value()) {
        leg.cms.cap = requireFiniteLevel(f->cap().value(), prefix + "cap");
    }
    if (f->floor().has_value()) {
        leg.cms.floor = requireFiniteLevel(f->floor().value(), prefix + "floor");
    }
    if (leg.cms.cap.has_value() && leg.cms.floor.has_value() &&
        leg.cms.cap.value() < leg.cms.floor.value()) {
        QUANTRA_INVALID_ARGUMENT(prefix + "cap must be >= floor");
    }
    leg.cms.pricerParams = decodeCmsPricer(f->pricer());
}

SwapLegData::Kind toKind(quantra::SwapCoupons kind) {
    switch (kind) {
        case quantra::SwapCoupons_FixedCoupons: return SwapLegData::Kind::Fixed;
        case quantra::SwapCoupons_IborCoupons: return SwapLegData::Kind::Ibor;
        case quantra::SwapCoupons_OvernightCoupons: return SwapLegData::Kind::Overnight;
        case quantra::SwapCoupons_CmsCoupons: return SwapLegData::Kind::Cms;
        default: break;
    }
    QUANTRA_INVALID_ARGUMENT("coupons_type is not supported");
    return SwapLegData::Kind::Fixed;
}

std::string allowedList(const SwapLegRules& rules) {
    std::string out;
    for (std::size_t i = 0; i < rules.allowedKinds.size(); ++i) {
        if (i > 0) out += (i + 1 == rules.allowedKinds.size()) ? " or " : ", ";
        out += SwapCouponsName(rules.allowedKinds[i]);
    }
    return out;
}

bool exchangeFlag(
    flatbuffers::Optional<bool> value, const std::string& field, const SwapLegRules& rules) {
    if (rules.exchangesRequired) {
        return requireBool(value, field);
    }
    const bool on = value.has_value() && value.value();
    if (on && !rules.exchangesAllowed) {
        QUANTRA_INVALID_ARGUMENT(
            field + " must be absent or false: a " + rules.endpoint +
            " leg carries no principal exchange");
    }
    return on;
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
            // A fixing that could not be read (NaN) is omitted, never emitted
            // as a bare `nan`.
            if (std::isfinite(f.indexFixing)) fb.add_index_fixing(f.indexFixing);
            if (f.hasCmsSwapRate && std::isfinite(f.cmsSwapRate)) {
                fb.add_cms_swap_rate(f.cmsSwapRate);
            }
            fb.add_spread(f.spread);
        }
    }
    return fb.Finish();
}

} // namespace

SwapLegData DecodeSwapLeg(
    const quantra::SwapLeg* fb, const std::string& name, const SwapLegRules& rules) {
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

    if (fb->coupons_type() == quantra::SwapCoupons_NONE) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".coupons is required (coupons_type " + allowedList(rules) +
            " with the matching coupons table)");
    }
    const SwapLegData::Kind kind = toKind(fb->coupons_type());
    const char* kindName = SwapCouponsName(kind);
    bool allowed = false;
    for (auto k : rules.allowedKinds)
        allowed = allowed || k == kind;
    if (!allowed) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".coupons must be " + allowedList(rules) + " (got " + kindName + ")");
    }
    if (fb->coupons() == nullptr) {
        QUANTRA_INVALID_ARGUMENT(
            name + ".coupons is required with coupons_type " + std::string(kindName));
    }

    leg.initialExchange = exchangeFlag(fb->initial_exchange(), name + ".initial_exchange", rules);
    leg.finalExchange = exchangeFlag(fb->final_exchange(), name + ".final_exchange", rules);
    leg.discountingCurveId = requireId(fb->discounting_curve(), name + ".discounting_curve");

    const bool floating = kind != SwapLegData::Kind::Fixed;
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

    const SharedLegFields shared = decodeShared(fb, name, rules, kind, leg);
    const std::string prefix = name + ".coupons.";
    switch (kind) {
        case SwapLegData::Kind::Fixed:
            decodeFixed(fb->coupons_as_FixedCoupons(), prefix, shared, leg);
            break;
        case SwapLegData::Kind::Ibor:
            decodeIbor(fb->coupons_as_IborCoupons(), prefix, shared, leg);
            break;
        case SwapLegData::Kind::Overnight:
            decodeOvernight(fb->coupons_as_OvernightCoupons(), prefix, shared, leg);
            break;
        case SwapLegData::Kind::Cms:
            decodeCms(fb->coupons_as_CmsCoupons(), name, shared, leg);
            break;
    }
    return leg;
}

void RequireOppositeSides(
    const SwapLegData& a,
    const std::string& nameA,
    const SwapLegData& b,
    const std::string& nameB) {
    if (a.side == b.side) {
        QUANTRA_INVALID_ARGUMENT(
            nameA + ".pay_receive and " + nameB + ".pay_receive are both " +
            (a.side == SwapLegData::Side::Pay ? "Pay" : "Receive") +
            ": one leg must be paid and the other received");
    }
}

void RequireOneCurrency(
    const SwapLegData& a,
    const std::string& nameA,
    const SwapLegData& b,
    const std::string& nameB,
    const std::string& endpoint) {
    if (a.currency != b.currency) {
        QUANTRA_INVALID_ARGUMENT(
            nameA + ".currency '" + a.currency + "' and " + nameB + ".currency '" + b.currency +
            "' must be equal on " + endpoint +
            " (two legs in two currencies are a cross-currency swap: use "
            "price-cross-currency-swap)");
    }
}

flatbuffers::Offset<quantra::SwapLegResult> SerializeSwapLegResult(
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

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegResult>>>
SerializeSwapLegResults(
    flatbuffers::grpc::MessageBuilder& builder,
    const std::vector<SwapLegPerLeg>& legs,
    bool includeFlows) {
    std::vector<flatbuffers::Offset<quantra::SwapLegResult>> offsets;
    offsets.reserve(legs.size());
    for (const auto& leg : legs) {
        offsets.push_back(SerializeSwapLegResult(builder, leg, includeFlows));
    }
    return builder.CreateVector(offsets);
}

} // namespace quantra
