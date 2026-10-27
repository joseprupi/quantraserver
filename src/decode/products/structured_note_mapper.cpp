#include "structured_note_mapper.h"

#include "date_convert.h"
#include "error.h"
#include "fx_spot_parser.h"          // RequireIsoCurrency
#include "implied_forwards_mapper.h" // decodeOptionQuoteSheet, decodeQuoteFilter

#include <cmath>
#include <set>
#include <string>

namespace quantra {

namespace {

using quantra::enums::BasketPerformanceRule_Average;
using quantra::enums::BasketPerformanceRule_BestOf;
using quantra::enums::BasketPerformanceRule_WorstOf;

/// Hard cap of `diagnostics.sample_paths` (the fbs doc).
constexpr unsigned int kMaxSamplePaths = 100;

std::string requireString(const flatbuffers::String* value, const std::string& field) {
    if (!value || value->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(field + ": required");
    }
    return value->str();
}

QuantLib::Date requireDate(const flatbuffers::String* value, const std::string& field) {
    return DateToQL(requireString(value, field));
}

double requireFinite(double v, const std::string& field) {
    if (!std::isfinite(v)) {
        QUANTRA_INVALID_ARGUMENT(field + ": must be finite");
    }
    return v;
}

double requirePresent(const flatbuffers::Optional<double>& v, const std::string& field) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(field + ": required");
    }
    return requireFinite(v.value(), field);
}

double requirePositive(const flatbuffers::Optional<double>& v, const std::string& field) {
    const double x = requirePresent(v, field);
    if (x <= 0.0) {
        QUANTRA_INVALID_ARGUMENT(field + ": must be > 0, got " + std::to_string(x));
    }
    return x;
}

double requireNonNegative(double v, const std::string& field) {
    requireFinite(v, field);
    if (v < 0.0) {
        QUANTRA_INVALID_ARGUMENT(field + ": must be >= 0, got " + std::to_string(v));
    }
    return v;
}

bool requireBool(const flatbuffers::Optional<bool>& v, const std::string& field) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(field + ": required");
    }
    return v.value();
}

BasketPerformanceRule toRule(
    const flatbuffers::Optional<quantra::enums::BasketPerformanceRule>& v,
    const std::string& field) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(field + ": required (WorstOf, BestOf or Average)");
    }
    switch (v.value()) {
        case BasketPerformanceRule_WorstOf: return BasketPerformanceRule::WorstOf;
        case BasketPerformanceRule_BestOf: return BasketPerformanceRule::BestOf;
        case BasketPerformanceRule_Average: return BasketPerformanceRule::Average;
    }
    QUANTRA_INVALID_ARGUMENT(field + ": unknown value");
    return BasketPerformanceRule::WorstOf;
}

quantra::enums::BasketPerformanceRule fromRule(BasketPerformanceRule r) {
    switch (r) {
        case BasketPerformanceRule::WorstOf: return BasketPerformanceRule_WorstOf;
        case BasketPerformanceRule::BestOf: return BasketPerformanceRule_BestOf;
        case BasketPerformanceRule::Average: return BasketPerformanceRule_Average;
    }
    return BasketPerformanceRule_WorstOf;
}

quantra::enums::CapitalBarrierMonitoring fromMonitoring(CapitalBarrierMonitoring m) {
    return m == CapitalBarrierMonitoring::European
               ? quantra::enums::CapitalBarrierMonitoring_European
               : quantra::enums::CapitalBarrierMonitoring_DailyClose;
}

quantra::enums::RandomSequence fromSequence(RandomSequenceKind s) {
    return s == RandomSequenceKind::Sobol ? quantra::enums::RandomSequence_Sobol
                                          : quantra::enums::RandomSequence_PseudoRandom;
}

quantra::enums::StructuredNoteStatus fromStatus(StructuredNoteStatus s) {
    switch (s) {
        case StructuredNoteStatus::Alive: return quantra::enums::StructuredNoteStatus_Alive;
        case StructuredNoteStatus::Called: return quantra::enums::StructuredNoteStatus_Called;
        case StructuredNoteStatus::Matured: return quantra::enums::StructuredNoteStatus_Matured;
    }
    return quantra::enums::StructuredNoteStatus_Alive;
}

BasketUnderlyingData decodeUnderlying(
    const quantra::BasketUnderlying* u, const std::string& path, const std::string& noteCurrency) {
    if (u == nullptr) {
        QUANTRA_INVALID_ARGUMENT(path + ": entry is null");
    }
    BasketUnderlyingData d;
    d.underlyingId = requireString(u->underlying_id(), path + ".underlying_id");
    d.volatilityId = requireString(u->volatility_id(), path + ".volatility_id");
    d.initialLevel = requirePositive(u->initial_level(), path + ".initial_level");
    if (const auto* q = u->quanto()) {
        const std::string qp = path + ".quanto";
        QuantoData qd;
        qd.discountingCurveId = requireString(q->discounting_curve(), qp + ".discounting_curve");
        qd.fxSpotId = requireString(q->fx_spot_id(), qp + ".fx_spot_id");
        qd.fxVolatilityId = requireString(q->fx_volatility_id(), qp + ".fx_volatility_id");
        qd.fxUnderlyingCorrelation =
            requirePresent(q->fx_underlying_correlation(), qp + ".fx_underlying_correlation");
        if (qd.fxUnderlyingCorrelation < -1.0 || qd.fxUnderlyingCorrelation > 1.0) {
            QUANTRA_INVALID_ARGUMENT(
                qp + ".fx_underlying_correlation: must be in [-1, 1], got " +
                std::to_string(qd.fxUnderlyingCorrelation));
        }
        if (qd.discountingCurveId.empty()) {
            QUANTRA_INVALID_ARGUMENT(qp + ".discounting_curve: required");
        }
        d.quanto = qd;
    }
    if (u->historical_levels()) {
        std::set<QuantLib::Date> seen;
        unsigned int j = 0;
        for (const auto* h : *u->historical_levels()) {
            const std::string hp = path + ".historical_levels[" + std::to_string(j++) + "]";
            if (h == nullptr) {
                QUANTRA_INVALID_ARGUMENT(hp + ": entry is null");
            }
            HistoricalLevel hl;
            hl.date = requireDate(h->date(), hp + ".date");
            hl.value = requireFinite(h->value(), hp + ".value");
            if (hl.value <= 0.0) {
                QUANTRA_INVALID_ARGUMENT(
                    hp + ".value: must be > 0, got " + std::to_string(hl.value));
            }
            if (!seen.insert(hl.date).second) {
                QUANTRA_INVALID_ARGUMENT(hp + ".date: duplicate date " + DateToIso(hl.date));
            }
            d.historicalLevels.push_back(hl);
        }
    }
    (void)noteCurrency; // the currency rule needs the underlying spec (evaluator)
    return d;
}

ObservationEventData decodeEvent(
    const quantra::ObservationEvent* ev, const std::string& path, bool isLast, bool memory) {
    if (ev == nullptr) {
        QUANTRA_INVALID_ARGUMENT(path + ": entry is null");
    }
    ObservationEventData d;
    d.observationDate = requireDate(ev->observation_date(), path + ".observation_date");
    if (ev->autocall_trigger().has_value()) {
        d.autocallTrigger =
            requireNonNegative(ev->autocall_trigger().value(), path + ".autocall_trigger");
    }
    if (ev->coupon_trigger().has_value()) {
        d.couponTrigger =
            requireNonNegative(ev->coupon_trigger().value(), path + ".coupon_trigger");
    }
    if (ev->coupon().has_value()) {
        d.coupon = requireNonNegative(ev->coupon().value(), path + ".coupon");
    }
    const bool hasTrigger = d.autocallTrigger || d.couponTrigger;
    if (ev->is_final().has_value() && ev->is_final().value()) {
        // Maturity-only observation: the capital block (and the upside) only.
        d.isFinal = true;
        if (!isLast) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".is_final: must be the last event (the final observation); this "
                       "event is followed by another one");
        }
        const char* present = d.autocallTrigger ? "autocall_trigger"
                              : d.couponTrigger ? "coupon_trigger"
                              : d.coupon        ? "coupon"
                                                : nullptr;
        if (present != nullptr) {
            QUANTRA_INVALID_ARGUMENT(
                path + "." + present +
                ": must be absent on an is_final event (a maturity-only observation has no "
                "coupon test and no call test; a final observation that tests a coupon or a "
                "call uses a trigger, not is_final)");
        }
        if (!ev->payment_date() || ev->payment_date()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".payment_date: required on an is_final event (the maturity payment "
                       "date, equal to maturity_payment_date)");
        }
        d.paymentDate = DateToQL(ev->payment_date()->str());
        if (*d.paymentDate < d.observationDate) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".payment_date: " + DateToIso(*d.paymentDate) +
                " is before observation_date " + DateToIso(d.observationDate));
        }
        return d;
    }
    if (hasTrigger) {
        if (!ev->payment_date() || ev->payment_date()->str().empty()) {
            QUANTRA_INVALID_ARGUMENT(path + ".payment_date: required when a trigger is present");
        }
        d.paymentDate = DateToQL(ev->payment_date()->str());
        if (*d.paymentDate < d.observationDate) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".payment_date: " + DateToIso(*d.paymentDate) +
                " is before observation_date " + DateToIso(d.observationDate));
        }
        if (d.couponTrigger && !d.coupon) {
            QUANTRA_INVALID_ARGUMENT(path + ".coupon: required when coupon_trigger is present");
        }
    } else {
        if (isLast) {
            QUANTRA_INVALID_ARGUMENT(
                path + ": the final event must carry an autocall_trigger or a coupon_trigger");
        }
        if (ev->payment_date()) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".payment_date: must be absent on an accrual-only event (no trigger)");
        }
        if (!d.coupon) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".coupon: required on an accrual-only event (no trigger)");
        }
        if (!memory) {
            QUANTRA_INVALID_ARGUMENT(
                path + ": an accrual-only event (no trigger) requires coupon_rule.memory = true "
                       "(nothing else could carry its coupon)");
        }
    }
    return d;
}

StructuredNoteTrade decodeNote(const quantra::StructuredNote* n, std::size_t i) {
    const std::string p = "notes[" + std::to_string(i) + "]";
    if (n == nullptr) {
        QUANTRA_INVALID_ARGUMENT(p + ": entry is null");
    }
    StructuredNoteTrade t;
    t.path = p;
    t.tradeId = requireString(n->trade_id(), p + ".trade_id");
    t.currency = RequireIsoCurrency(n->currency(), p + ".currency");
    t.notional = requirePositive(n->notional(), p + ".notional");

    if (!n->underlyings() || n->underlyings()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(p + ".underlyings: required, at least one entry");
    }
    std::set<std::string> ids;
    for (unsigned int k = 0; k < n->underlyings()->size(); ++k) {
        auto d = decodeUnderlying(
            n->underlyings()->Get(k), p + ".underlyings[" + std::to_string(k) + "]", t.currency);
        if (!ids.insert(d.underlyingId).second) {
            QUANTRA_INVALID_ARGUMENT(
                p + ".underlyings[" + std::to_string(k) + "].underlying_id: duplicate '" +
                d.underlyingId + "'");
        }
        t.underlyings.push_back(d);
    }
    t.performanceRule = toRule(n->performance_rule(), p + ".performance_rule");

    if (!n->coupon_rule()) {
        QUANTRA_INVALID_ARGUMENT(p + ".coupon_rule: required");
    }
    t.couponRule.memory = requireBool(n->coupon_rule()->memory(), p + ".coupon_rule.memory");
    t.couponRule.payOnlyOnCall =
        requireBool(n->coupon_rule()->pay_only_on_call(), p + ".coupon_rule.pay_only_on_call");

    if (!n->events() || n->events()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(p + ".events: required, at least one entry");
    }
    const unsigned int nE = n->events()->size();
    for (unsigned int j = 0; j < nE; ++j) {
        const std::string ep = p + ".events[" + std::to_string(j) + "]";
        auto d = decodeEvent(n->events()->Get(j), ep, j + 1 == nE, t.couponRule.memory);
        if (j > 0 && !(d.observationDate > t.events.back().observationDate)) {
            QUANTRA_INVALID_ARGUMENT(
                ep + ".observation_date: " + DateToIso(d.observationDate) +
                " must be after events[" + std::to_string(j - 1) + "].observation_date " +
                DateToIso(t.events.back().observationDate) + " (strictly increasing)");
        }
        t.events.push_back(d);
    }

    if (!n->capital()) {
        QUANTRA_INVALID_ARGUMENT(p + ".capital: required");
    }
    {
        const auto* c = n->capital();
        const std::string cp = p + ".capital";
        t.capital.barrier = requirePresent(c->barrier(), cp + ".barrier");
        if (t.capital.barrier <= 0.0 || t.capital.barrier > 1.0) {
            QUANTRA_INVALID_ARGUMENT(
                cp + ".barrier: must be in (0, 1], got " + std::to_string(t.capital.barrier));
        }
        if (!c->monitoring().has_value()) {
            QUANTRA_INVALID_ARGUMENT(cp + ".monitoring: required (European or DailyClose)");
        }
        t.capital.monitoring =
            c->monitoring().value() == quantra::enums::CapitalBarrierMonitoring_DailyClose
                ? CapitalBarrierMonitoring::DailyClose
                : CapitalBarrierMonitoring::European;
        t.capital.putStrike = requireNonNegative(c->put_strike(), cp + ".put_strike");
        t.capital.participation = requireNonNegative(c->participation(), cp + ".participation");
        if (c->knocked_in().has_value()) t.capital.knockedIn = c->knocked_in().value();
    }

    t.maturityPaymentDate = requireDate(n->maturity_payment_date(), p + ".maturity_payment_date");
    if (t.maturityPaymentDate < t.events.back().observationDate) {
        QUANTRA_INVALID_ARGUMENT(
            p + ".maturity_payment_date: " + DateToIso(t.maturityPaymentDate) +
            " is before the final observation_date " + DateToIso(t.events.back().observationDate));
    }
    if (t.events.back().isFinal && *t.events.back().paymentDate != t.maturityPaymentDate) {
        QUANTRA_INVALID_ARGUMENT(
            p + ".events[" + std::to_string(nE - 1) +
            "].payment_date: " + DateToIso(*t.events.back().paymentDate) +
            " must equal maturity_payment_date " + DateToIso(t.maturityPaymentDate) +
            " on an is_final event (the redemption is the only payment of a maturity-only "
            "observation)");
    }
    t.discountingCurveId = requireString(n->discounting_curve(), p + ".discounting_curve");
    t.fundingSpreadBp = requireFinite(n->funding_spread_bp(), p + ".funding_spread_bp");
    if (n->correlation_id() && !n->correlation_id()->str().empty()) {
        t.correlationId = n->correlation_id()->str();
    }
    if (t.underlyings.size() == 1 && !t.correlationId.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            p + ".correlation_id: must be absent with a single underlying (a 1 x 1 unit matrix "
                "is used; a correlation matrix lists at least two labels)");
    }
    if (t.underlyings.size() > 1 && t.correlationId.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            p + ".correlation_id: required with " + std::to_string(t.underlyings.size()) +
            " underlyings");
    }
    if (n->correlation_markup().has_value()) {
        const double markup = n->correlation_markup().value();
        if (!std::isfinite(markup) || markup < -1.0 || markup > 1.0) {
            QUANTRA_INVALID_ARGUMENT(
                p + ".correlation_markup: must be finite and in [-1, 1], got " +
                std::to_string(markup));
        }
        if (t.underlyings.size() == 1) {
            QUANTRA_INVALID_ARGUMENT(
                p + ".correlation_markup: must be absent with a single underlying (there is no "
                    "off-diagonal correlation to shift)");
        }
        t.correlationMarkup = markup;
    }

    if (const auto* up = n->upside()) {
        const std::string upp = p + ".upside";
        UpsideData u;
        u.strike = requirePresent(up->strike(), upp + ".strike");
        if (u.strike < 0.0) {
            QUANTRA_INVALID_ARGUMENT(
                upp + ".strike: must be >= 0, got " + std::to_string(u.strike));
        }
        u.participation = requirePresent(up->participation(), upp + ".participation");
        if (u.participation < 0.0) {
            QUANTRA_INVALID_ARGUMENT(
                upp + ".participation: must be >= 0, got " + std::to_string(u.participation));
        }
        if (up->cap().has_value()) {
            u.cap = requirePositive(up->cap(), upp + ".cap");
        }
        t.upside = u;
    }

    if (!n->monte_carlo()) {
        QUANTRA_INVALID_ARGUMENT(p + ".monte_carlo: required");
    }
    {
        const auto* m = n->monte_carlo();
        const std::string mp = p + ".monte_carlo";
        if (!m->paths().has_value()) {
            QUANTRA_INVALID_ARGUMENT(mp + ".paths: required");
        }
        t.monteCarlo.paths = m->paths().value();
        t.monteCarlo.seed = static_cast<unsigned long>(m->seed());
        t.monteCarlo.sequence = m->sequence() == quantra::enums::RandomSequence_PseudoRandom
                                    ? RandomSequenceKind::PseudoRandom
                                    : RandomSequenceKind::Sobol;
        t.monteCarlo.antithetic = m->antithetic();
        t.monteCarlo.brownianBridge = m->brownian_bridge();
        if (t.monteCarlo.paths == 0) {
            QUANTRA_INVALID_ARGUMENT(mp + ".paths: must be > 0");
        }
        if (t.monteCarlo.antithetic && t.monteCarlo.paths % 2 != 0) {
            QUANTRA_INVALID_ARGUMENT(
                mp +
                ".paths: must be even with antithetic = true (antithetic pairs count as "
                "two paths), got " +
                std::to_string(t.monteCarlo.paths));
        }
        if (t.monteCarlo.sequence == RandomSequenceKind::Sobol && t.monteCarlo.seed == 0) {
            QUANTRA_INVALID_ARGUMENT(
                mp + ".seed: must be > 0 with the Sobol sequence (QuantLib treats seed 0 as "
                     "clock-based for the direction integers beyond its table, which would make "
                     "the price irreproducible)");
        }
        if (t.monteCarlo.sequence == RandomSequenceKind::PseudoRandom &&
            t.monteCarlo.brownianBridge) {
            QUANTRA_INVALID_ARGUMENT(
                mp + ".brownian_bridge: not supported with the PseudoRandom sequence (the bridge "
                     "is an ordering of low-discrepancy dimensions and changes nothing for "
                     "independent draws); set it to false");
        }
        if (m->steps_per_year().has_value()) {
            if (m->steps_per_year().value() == 0) {
                QUANTRA_INVALID_ARGUMENT(mp + ".steps_per_year: must be > 0 when present");
            }
            t.monteCarlo.stepsPerYear = m->steps_per_year().value();
        }
        if (t.capital.monitoring == CapitalBarrierMonitoring::DailyClose &&
            !t.monteCarlo.stepsPerYear) {
            QUANTRA_INVALID_ARGUMENT(
                mp + ".steps_per_year: required, the capital barrier is monitored DailyClose");
        }
    }
    return t;
}

// ---------------------------------------------------------------------------
// Response
// ---------------------------------------------------------------------------

flatbuffers::Offset<quantra::StructuredNoteTermsEcho> serializeTerms(
    flatbuffers::grpc::MessageBuilder& b, const NoteTermsEcho& t) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteTermsUnderlying>> unds;
    for (const auto& u : t.underlyings) {
        auto id = b.CreateString(u.underlyingId);
        auto ccy = b.CreateString(u.currency);
        auto vol = b.CreateString(u.volatilityId);
        flatbuffers::Offset<flatbuffers::String> fxSpot, fxVol;
        if (u.quanto) {
            fxSpot = b.CreateString(u.fxSpotId);
            fxVol = b.CreateString(u.fxVolatilityId);
        }
        quantra::StructuredNoteTermsUnderlyingBuilder ub(b);
        ub.add_underlying_id(id);
        ub.add_currency(ccy);
        ub.add_initial_level(u.initialLevel);
        ub.add_volatility_id(vol);
        ub.add_quanto(u.quanto);
        if (u.quanto) {
            ub.add_fx_spot_id(fxSpot);
            ub.add_fx_volatility_id(fxVol);
            if (u.fxUnderlyingCorrelation) {
                ub.add_fx_underlying_correlation(*u.fxUnderlyingCorrelation);
            }
        }
        unds.push_back(ub.Finish());
    }
    std::vector<flatbuffers::Offset<quantra::StructuredNoteTermsEvent>> evs;
    for (const auto& e : t.events) {
        auto obs = b.CreateString(e.observationDate);
        flatbuffers::Offset<flatbuffers::String> pay;
        if (e.paymentDate) pay = b.CreateString(*e.paymentDate);
        quantra::StructuredNoteTermsEventBuilder eb(b);
        eb.add_index(e.index);
        eb.add_observation_date(obs);
        if (e.paymentDate) eb.add_payment_date(pay);
        if (e.autocallTrigger) eb.add_autocall_trigger(*e.autocallTrigger);
        if (e.couponTrigger) eb.add_coupon_trigger(*e.couponTrigger);
        if (e.coupon) eb.add_coupon(*e.coupon);
        eb.add_accrual_only(e.accrualOnly);
        eb.add_final(e.final);
        eb.add_is_final(e.isFinal);
        eb.add_resolved_from_history(e.resolvedFromHistory);
        evs.push_back(eb.Finish());
    }
    auto undsVec = b.CreateVector(unds);
    auto evsVec = b.CreateVector(evs);
    auto tradeId = b.CreateString(t.tradeId);
    auto ccy = b.CreateString(t.currency);
    auto mat = b.CreateString(t.maturityPaymentDate);
    auto disc = b.CreateString(t.discountingCurveId);
    flatbuffers::Offset<flatbuffers::String> corr;
    if (!t.correlationId.empty()) corr = b.CreateString(t.correlationId);
    quantra::StructuredNoteTermsEchoBuilder tb(b);
    tb.add_trade_id(tradeId);
    tb.add_currency(ccy);
    tb.add_notional(t.notional);
    tb.add_performance_rule(fromRule(t.performanceRule));
    tb.add_underlyings(undsVec);
    tb.add_events(evsVec);
    tb.add_coupon_memory(t.couponMemory);
    tb.add_coupon_pay_only_on_call(t.couponPayOnlyOnCall);
    tb.add_capital_barrier(t.capitalBarrier);
    tb.add_capital_monitoring(fromMonitoring(t.capitalMonitoring));
    tb.add_put_strike(t.putStrike);
    tb.add_participation(t.participation);
    tb.add_maturity_payment_date(mat);
    tb.add_discounting_curve(disc);
    tb.add_funding_spread_bp(t.fundingSpreadBp);
    if (!t.correlationId.empty()) tb.add_correlation_id(corr);
    if (t.correlationMarkup) tb.add_correlation_markup(*t.correlationMarkup);
    tb.add_correlation_markup_repaired(t.correlationMarkupRepaired);
    tb.add_paths(t.paths);
    tb.add_seed(static_cast<uint64_t>(t.seed));
    tb.add_sequence(fromSequence(t.sequence));
    tb.add_antithetic(t.antithetic);
    tb.add_brownian_bridge(t.brownianBridge);
    if (t.stepsPerYear) tb.add_steps_per_year(*t.stepsPerYear);
    return tb.Finish();
}

flatbuffers::Offset<quantra::StructuredNoteSeasoning> serializeSeasoning(
    flatbuffers::grpc::MessageBuilder& b, const NoteSeasoning& s) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteResolvedEvent>> evs;
    for (const auto& e : s.events) {
        auto obs = b.CreateString(e.observationDate);
        auto levels = b.CreateVector(e.levels);
        auto perfs = b.CreateVector(e.performances);
        quantra::StructuredNoteResolvedEventBuilder eb(b);
        eb.add_index(e.index);
        eb.add_observation_date(obs);
        eb.add_accrual_only(e.accrualOnly);
        eb.add_levels(levels);
        eb.add_performances(perfs);
        if (e.basketPerformance) eb.add_basket_performance(*e.basketPerformance);
        if (e.called) eb.add_called(*e.called);
        if (e.couponEarned) eb.add_coupon_earned(*e.couponEarned);
        eb.add_coupon_paid(e.couponPaid);
        eb.add_memory_after(e.memoryAfter);
        evs.push_back(eb.Finish());
    }
    auto evsVec = b.CreateVector(evs);
    auto asOf = b.CreateString(s.asOfDate);
    flatbuffers::Offset<flatbuffers::String> known;
    if (s.knownPaymentDate) known = b.CreateString(*s.knownPaymentDate);
    quantra::StructuredNoteSeasoningBuilder sb(b);
    sb.add_as_of_date(asOf);
    sb.add_status(fromStatus(s.status));
    sb.add_events(evsVec);
    sb.add_memory_carried(s.memoryCarried);
    if (s.nextEventIndex) sb.add_next_event_index(*s.nextEventIndex);
    if (s.knownPaymentDate) sb.add_known_payment_date(known);
    if (s.knownPayment) sb.add_known_payment(*s.knownPayment);
    return sb.Finish();
}

flatbuffers::Offset<quantra::StructuredNoteModelEcho> serializeModel(
    flatbuffers::grpc::MessageBuilder& b, const NoteModelEcho& m) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteModelUnderlying>> unds;
    for (const auto& u : m.underlyings) {
        auto id = b.CreateString(u.underlyingId);
        quantra::StructuredNoteModelUnderlyingBuilder ub(b);
        ub.add_underlying_id(id);
        ub.add_vol_shape(u.volShape);
        ub.add_local_vol(u.localVol);
        if (u.quantoDrift) ub.add_quanto_drift(*u.quantoDrift);
        unds.push_back(ub.Finish());
    }
    auto undsVec = b.CreateVector(unds);
    auto grid = b.CreateVector(m.timeGrid);
    quantra::StructuredNoteModelEchoBuilder mb(b);
    mb.add_underlyings(undsVec);
    mb.add_observation_dates_only(m.observationDatesOnly);
    mb.add_time_steps(m.timeSteps);
    mb.add_time_grid(grid);
    mb.add_sequence_dimension(m.sequenceDimension);
    return mb.Finish();
}

flatbuffers::Offset<quantra::StructuredNoteBucket> serializeBucket(
    flatbuffers::grpc::MessageBuilder& b, const NoteBucket& k) {
    return quantra::CreateStructuredNoteBucket(b, k.pv, k.standardError);
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>> strings(
    flatbuffers::grpc::MessageBuilder& b, const std::vector<std::string>& v) {
    std::vector<flatbuffers::Offset<flatbuffers::String>> out;
    out.reserve(v.size());
    for (const auto& s : v)
        out.push_back(b.CreateString(s));
    return b.CreateVector(out);
}

flatbuffers::Offset<quantra::StructuredNoteMarketDiagnostics> serializeMarket(
    flatbuffers::grpc::MessageBuilder& b, const NoteMarketDiagnostics& m) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteMarketUnderlying>> unds;
    for (const auto& u : m.underlyings) {
        auto id = b.CreateString(u.underlyingId);
        auto fwd = b.CreateVector(u.forwards);
        auto divs = b.CreateVector(u.impliedDividendYields);
        auto rates = b.CreateVector(u.impliedRates);
        auto atm = b.CreateVector(u.atmVols);
        auto bar = b.CreateVector(u.barrierVols);
        quantra::StructuredNoteMarketUnderlyingBuilder ub(b);
        ub.add_underlying_id(id);
        ub.add_spot(u.spot);
        ub.add_forwards(fwd);
        ub.add_implied_dividend_yields(divs);
        ub.add_implied_rates(rates);
        ub.add_atm_vols(atm);
        ub.add_barrier_vols(bar);
        if (u.quantoDriftBp) ub.add_quanto_drift_bp(*u.quantoDriftBp);
        unds.push_back(ub.Finish());
    }
    auto obs = strings(b, m.observationDates);
    auto undsVec = b.CreateVector(unds);
    auto pay = strings(b, m.paymentDates);
    auto dfs = b.CreateVector(m.discountFactors);
    auto dfsF = b.CreateVector(m.discountFactorsWithFunding);
    auto labels = strings(b, m.correlationLabels);
    auto corr = b.CreateVector(m.correlation);
    flatbuffers::Offset<flatbuffers::Vector<double>> corrBase;
    if (!m.correlationBase.empty()) corrBase = b.CreateVector(m.correlationBase);
    quantra::StructuredNoteMarketDiagnosticsBuilder mb(b);
    mb.add_observation_dates(obs);
    mb.add_underlyings(undsVec);
    mb.add_payment_dates(pay);
    mb.add_discount_factors(dfs);
    mb.add_discount_factors_with_funding(dfsF);
    mb.add_correlation_labels(labels);
    mb.add_correlation(corr);
    if (!m.correlationBase.empty()) mb.add_correlation_base(corrBase);
    if (m.correlationMarkup) mb.add_correlation_markup(*m.correlationMarkup);
    return mb.Finish();
}

flatbuffers::Offset<quantra::StructuredNoteDecompositionDiagnostics> serializeDecomposition(
    flatbuffers::grpc::MessageBuilder& b, const NoteDecompositionDiagnostics& d) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteEventDecomposition>> evs;
    for (const auto& e : d.events) {
        auto obs = b.CreateString(e.observationDate);
        quantra::StructuredNoteEventDecompositionBuilder eb(b);
        eb.add_index(e.index);
        eb.add_observation_date(obs);
        if (e.pCall) eb.add_p_call(*e.pCall);
        if (e.pCoupon) eb.add_p_coupon(*e.pCoupon);
        eb.add_pv_coupons(e.pvCoupons);
        eb.add_pv_redemption(e.pvRedemption);
        evs.push_back(eb.Finish());
    }
    auto evsVec = b.CreateVector(evs);
    quantra::StructuredNoteDecompositionDiagnosticsBuilder db(b);
    db.add_events(evsVec);
    db.add_p_survive_to_maturity(d.pSurviveToMaturity);
    db.add_p_breach(d.pBreach);
    db.add_expected_life_years(d.expectedLifeYears);
    if (d.expectedLossGivenBreach) db.add_expected_loss_given_breach(*d.expectedLossGivenBreach);
    return db.Finish();
}

flatbuffers::Offset<quantra::StructuredNoteChecksDiagnostics> serializeChecks(
    flatbuffers::grpc::MessageBuilder& b, const NoteChecksDiagnostics& c) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteRunningMean>> rms;
    for (const auto& r : c.runningMeans) {
        rms.push_back(
            quantra::CreateStructuredNoteRunningMean(
                b, r.fraction, r.paths, r.npv, r.standardError));
    }
    auto rmsVec = b.CreateVector(rms);
    quantra::StructuredNoteChecksDiagnosticsBuilder cb(b);
    cb.add_running_means(rmsVec);
    cb.add_deterministic_forward_npv(c.deterministicForwardNpv);
    cb.add_correlation_one_npv(c.correlationOneNpv);
    cb.add_correlation_one_standard_error(c.correlationOneStandardError);
    cb.add_correlation_zero_npv(c.correlationZeroNpv);
    cb.add_correlation_zero_standard_error(c.correlationZeroStandardError);
    cb.add_bond_only_npv(c.bondOnlyNpv);
    return cb.Finish();
}

flatbuffers::Offset<quantra::StructuredNoteDistributionDiagnostics> serializeDistribution(
    flatbuffers::grpc::MessageBuilder& b, const NoteDistributionDiagnostics& d) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteQuantiles>> qs;
    for (const auto& q : d.basketPerformanceQuantiles) {
        auto obs = b.CreateString(q.observationDate);
        qs.push_back(
            quantra::CreateStructuredNoteQuantiles(b, obs, q.q05, q.q25, q.q50, q.q75, q.q95));
    }
    std::vector<flatbuffers::Offset<quantra::StructuredNoteHistogramBin>> bins;
    for (const auto& h : d.payoutHistogram) {
        bins.push_back(quantra::CreateStructuredNoteHistogramBin(b, h.lower, h.upper, h.paths));
    }
    std::vector<flatbuffers::Offset<quantra::StructuredNoteSamplePath>> sps;
    for (const auto& s : d.samplePaths) {
        auto perfs = b.CreateVector(s.basketPerformances);
        flatbuffers::Offset<flatbuffers::String> calledOn;
        if (s.calledOn) calledOn = b.CreateString(*s.calledOn);
        quantra::StructuredNoteSamplePathBuilder sb(b);
        sb.add_basket_performances(perfs);
        sb.add_payout(s.payout);
        sb.add_payout_pv(s.payoutPv);
        if (s.calledOn) sb.add_called_on(calledOn);
        sb.add_breached(s.breached);
        sps.push_back(sb.Finish());
    }
    auto qsVec = b.CreateVector(qs);
    auto binsVec = b.CreateVector(bins);
    auto spsVec = b.CreateVector(sps);
    quantra::StructuredNoteDistributionDiagnosticsBuilder db(b);
    db.add_basket_performance_quantiles(qsVec);
    db.add_payout_q05(d.payoutQ05);
    db.add_payout_q25(d.payoutQ25);
    db.add_payout_q50(d.payoutQ50);
    db.add_payout_q75(d.payoutQ75);
    db.add_payout_q95(d.payoutQ95);
    db.add_payout_histogram(binsVec);
    db.add_sample_paths(spsVec);
    return db.Finish();
}

flatbuffers::Offset<quantra::StructuredNoteGreeks> serializeGreeks(
    flatbuffers::grpc::MessageBuilder& b, const NoteGreeks& g) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteUnderlyingGreeks>> rows;
    for (const auto& u : g.underlyings) {
        auto id = b.CreateString(u.underlyingId);
        quantra::StructuredNoteUnderlyingGreeksBuilder ub(b);
        ub.add_underlying_id(id);
        ub.add_delta(u.delta.value);
        ub.add_delta_standard_error(u.delta.standardError);
        ub.add_gamma(u.gamma.value);
        ub.add_gamma_standard_error(u.gamma.standardError);
        ub.add_vega(u.vega.value);
        ub.add_vega_standard_error(u.vega.standardError);
        rows.push_back(ub.Finish());
    }
    auto rowsVec = b.CreateVector(rows);
    flatbuffers::Offset<flatbuffers::Vector<double>> matrix;
    if (!g.correlationBumped.empty()) matrix = b.CreateVector(g.correlationBumped);
    quantra::StructuredNoteGreeksBuilder gb(b);
    gb.add_underlyings(rowsVec);
    if (g.correlationSensitivity) {
        gb.add_correlation_sensitivity(g.correlationSensitivity->value);
        gb.add_correlation_sensitivity_standard_error(g.correlationSensitivity->standardError);
    }
    gb.add_correlation_bump_repaired(g.correlationBumpRepaired);
    if (!g.correlationBumped.empty()) gb.add_correlation_bumped(matrix);
    gb.add_funding_dv01(g.fundingDv01.value);
    gb.add_funding_dv01_standard_error(g.fundingDv01.standardError);
    gb.add_rho(g.rho.value);
    gb.add_rho_standard_error(g.rho.standardError);
    gb.add_spot_bump(g.spotBump);
    gb.add_vol_bump(g.volBump);
    gb.add_correlation_bump(g.correlationBump);
    gb.add_funding_bump_bp(g.fundingBumpBp);
    gb.add_rate_bump_bp(g.rateBumpBp);
    return gb.Finish();
}

flatbuffers::Offset<quantra::StructuredNoteTimingDiagnostics> serializeTiming(
    flatbuffers::grpc::MessageBuilder& b, const NoteTimingDiagnostics& t) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteTimingStage>> stages;
    for (const auto& s : t.stages) {
        auto name = b.CreateString(s.stage);
        stages.push_back(quantra::CreateStructuredNoteTimingStage(b, name, s.milliseconds));
    }
    auto stagesVec = b.CreateVector(stages);
    return quantra::CreateStructuredNoteTimingDiagnostics(b, stagesVec, t.totalMilliseconds);
}

} // namespace

StructuredNoteInputs StructuredNoteMapper::toInputs(
    const quantra::PriceStructuredNoteRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceStructuredNoteRequest is null");
    }
    if (req->pricing() == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceStructuredNoteRequest.pricing: required");
    }
    if (req->notes() == nullptr || req->notes()->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("PriceStructuredNoteRequest.notes: required, at least one entry");
    }
    StructuredNoteInputs inputs;
    if (const auto* gs = req->greeks()) {
        NoteGreeksRequest g;
        const auto bump = [](double v, const char* field) {
            if (!std::isfinite(v) || !(v > 0.0)) {
                QUANTRA_INVALID_ARGUMENT(
                    std::string("PriceStructuredNoteRequest.greeks.") + field +
                    ": must be finite and > 0, got " + std::to_string(v));
            }
            return v;
        };
        g.spotBump = bump(gs->spot_bump(), "spot_bump");
        g.volBump = bump(gs->vol_bump(), "vol_bump");
        g.correlationBump = bump(gs->correlation_bump(), "correlation_bump");
        g.fundingBumpBp = bump(gs->funding_bump_bp(), "funding_bump_bp");
        g.rateBumpBp = bump(gs->rate_bump_bp(), "rate_bump_bp");
        inputs.greeks = g;
    }
    if (const auto* d = req->diagnostics()) {
        NoteDiagnosticsRequest& g = inputs.diagnostics;
        g.market = d->market();
        g.decomposition = d->decomposition();
        g.checks = d->checks();
        g.distribution = d->distribution();
        g.timing = d->timing();
        g.samplePaths = d->sample_paths();
        if (g.samplePaths > kMaxSamplePaths) {
            QUANTRA_INVALID_ARGUMENT(
                "PriceStructuredNoteRequest.diagnostics.sample_paths: must be <= " +
                std::to_string(kMaxSamplePaths) + ", got " + std::to_string(g.samplePaths));
        }
        if (g.samplePaths > 0 && !g.distribution) {
            QUANTRA_INVALID_ARGUMENT(
                "PriceStructuredNoteRequest.diagnostics.sample_paths: requires distribution = "
                "true (the paths are returned in the distribution block)");
        }
        g.vanillaRepricing = d->vanilla_repricing();
        const auto* sheets = d->vanilla_repricing_sheets();
        const bool hasSheets = sheets && sheets->size() > 0;
        if (g.vanillaRepricing && !hasSheets) {
            QUANTRA_INVALID_ARGUMENT(
                "PriceStructuredNoteRequest.diagnostics.vanilla_repricing_sheets: required with "
                "vanilla_repricing = true (the listed-option quotes to reprice through the "
                "simulation)");
        }
        if (!g.vanillaRepricing && (hasSheets || d->vanilla_repricing_filter())) {
            QUANTRA_INVALID_ARGUMENT(
                "PriceStructuredNoteRequest.diagnostics.vanilla_repricing_sheets: requires "
                "vanilla_repricing = true");
        }
        if (g.vanillaRepricing) {
            std::set<std::string> seen;
            for (unsigned int i = 0; i < sheets->size(); ++i) {
                const std::string path =
                    "PriceStructuredNoteRequest.diagnostics.vanilla_repricing_sheets[" +
                    std::to_string(i) + "]";
                DecodedQuoteSheet decoded = decodeOptionQuoteSheet(sheets->Get(i), path);
                if (!seen.insert(decoded.underlyingId).second) {
                    QUANTRA_INVALID_ARGUMENT(
                        path + ".underlying_id: '" + decoded.underlyingId +
                        "' already has a sheet (one sheet per underlying)");
                }
                NoteRepricingSheet sheet;
                sheet.sheetId = std::move(decoded.sheetId);
                sheet.underlyingId = std::move(decoded.underlyingId);
                sheet.style = decoded.style;
                sheet.quotes = std::move(decoded.quotes);
                sheet.path = path;
                g.repricingSheets.push_back(std::move(sheet));
            }
            g.repricingFilter = decodeQuoteFilter(d->vanilla_repricing_filter());
            validateQuoteFilter(g.repricingFilter);
        }
    }
    inputs.notes.reserve(req->notes()->size());
    for (unsigned int i = 0; i < req->notes()->size(); ++i) {
        inputs.notes.push_back(decodeNote(req->notes()->Get(i), i));
    }
    return inputs;
}

flatbuffers::Offset<quantra::StructuredNoteVanillaRepricing> serializeVanillaRepricing(
    flatbuffers::grpc::MessageBuilder& b, const NoteVanillaRepricing& v) {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteVanillaRepricingUnderlying>> unds;
    for (const auto& u : v.underlyings) {
        std::vector<flatbuffers::Offset<quantra::StructuredNoteRepricingExpiry>> exps;
        for (const auto& e : u.expiries) {
            std::vector<flatbuffers::Offset<quantra::DroppedQuote>> dropped;
            for (const auto& d : e.dropped) {
                auto exp = b.CreateString(d.expiryIso);
                auto reason = b.CreateString(d.reason);
                quantra::DroppedQuoteBuilder db(b);
                db.add_expiry(exp);
                if (d.strike) db.add_strike(*d.strike);
                db.add_reason(reason);
                dropped.push_back(db.Finish());
            }
            auto droppedVec = b.CreateVector(dropped);
            auto exp = b.CreateString(e.expiry);
            quantra::StructuredNoteRepricingExpiryBuilder eb(b);
            eb.add_expiry(exp);
            eb.add_time(e.time);
            eb.add_forward(e.forward);
            eb.add_discount_factor(e.discountFactor);
            eb.add_quotes_used(e.quotesUsed);
            eb.add_quotes_dropped(static_cast<unsigned int>(e.dropped.size()));
            eb.add_dropped(droppedVec);
            exps.push_back(eb.Finish());
        }
        std::vector<flatbuffers::Offset<quantra::StructuredNoteRepricedOption>> opts;
        for (const auto& o : u.options) {
            auto exp = b.CreateString(o.expiry);
            quantra::StructuredNoteRepricedOptionBuilder ob(b);
            ob.add_expiry(exp);
            ob.add_time(o.time);
            ob.add_strike(o.strike);
            ob.add_option_type(
                o.isCall ? quantra::enums::EquityOptionType_Call
                         : quantra::enums::EquityOptionType_Put);
            ob.add_bid(o.bid);
            ob.add_ask(o.ask);
            ob.add_mid(o.mid);
            ob.add_model_price(o.modelPrice);
            ob.add_standard_error(o.standardError);
            ob.add_inside_bid_ask(o.insideBidAsk);
            ob.add_price_error(o.priceError);
            if (o.marketVol) ob.add_market_vol(*o.marketVol);
            if (o.modelVol) ob.add_model_vol(*o.modelVol);
            if (o.volError) ob.add_vol_error(*o.volError);
            opts.push_back(ob.Finish());
        }
        auto expsVec = b.CreateVector(exps);
        auto optsVec = b.CreateVector(opts);
        auto id = b.CreateString(u.underlyingId);
        auto sheetId = b.CreateString(u.sheetId);
        quantra::StructuredNoteVanillaRepricingUnderlyingBuilder ub(b);
        ub.add_underlying_id(id);
        ub.add_sheet_id(sheetId);
        ub.add_spot(u.spot);
        ub.add_expiries(expsVec);
        ub.add_count(u.count);
        ub.add_inside_bid_ask(u.insideBidAsk);
        ub.add_max_abs_price_error(u.maxAbsPriceError);
        if (u.maxAbsVolError) ub.add_max_abs_vol_error(*u.maxAbsVolError);
        ub.add_max_standard_error(u.maxStandardError);
        ub.add_options(optsVec);
        unds.push_back(ub.Finish());
    }
    auto undsVec = b.CreateVector(unds);
    quantra::StructuredNoteVanillaRepricingBuilder vb(b);
    vb.add_paths(v.paths);
    vb.add_time_steps(v.timeSteps);
    vb.add_note_npv(v.noteNpv);
    vb.add_note_standard_error(v.noteStandardError);
    vb.add_count(v.count);
    vb.add_inside_bid_ask(v.insideBidAsk);
    vb.add_max_abs_price_error(v.maxAbsPriceError);
    if (v.maxAbsVolError) vb.add_max_abs_vol_error(*v.maxAbsVolError);
    vb.add_max_standard_error(v.maxStandardError);
    vb.add_underlyings(undsVec);
    return vb.Finish();
}

flatbuffers::Offset<quantra::PriceStructuredNoteResponse> StructuredNoteMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& b, const NoteResult& result) const {
    std::vector<flatbuffers::Offset<quantra::StructuredNoteResult>> rows;
    rows.reserve(result.notes.size());
    for (const auto& r : result.notes) {
        auto tradeId = b.CreateString(r.tradeId);
        auto terms = serializeTerms(b, r.terms);
        auto seasoning = serializeSeasoning(b, r.seasoning);
        auto model = serializeModel(b, r.model);
        flatbuffers::Offset<quantra::StructuredNoteLegs> legs;
        if (r.legs) {
            auto bond = serializeBucket(b, r.legs->bondRedemption);
            auto coupons = serializeBucket(b, r.legs->coupons);
            auto loss = serializeBucket(b, r.legs->capitalLoss);
            legs = quantra::CreateStructuredNoteLegs(b, bond, coupons, loss);
        }
        std::vector<flatbuffers::Offset<quantra::StructuredNoteEventProbability>> probs;
        for (const auto& p : r.eventProbabilities) {
            auto obs = b.CreateString(p.observationDate);
            quantra::StructuredNoteEventProbabilityBuilder pb(b);
            pb.add_index(p.index);
            pb.add_observation_date(obs);
            if (p.pCall) pb.add_p_call(*p.pCall);
            if (p.pCoupon) pb.add_p_coupon(*p.pCoupon);
            probs.push_back(pb.Finish());
        }
        auto probsVec = b.CreateVector(probs);
        flatbuffers::Offset<quantra::StructuredNoteMarketDiagnostics> market;
        if (r.market) market = serializeMarket(b, *r.market);
        flatbuffers::Offset<quantra::StructuredNoteDecompositionDiagnostics> decomposition;
        if (r.decomposition) decomposition = serializeDecomposition(b, *r.decomposition);
        flatbuffers::Offset<quantra::StructuredNoteChecksDiagnostics> checks;
        if (r.checks) checks = serializeChecks(b, *r.checks);
        flatbuffers::Offset<quantra::StructuredNoteDistributionDiagnostics> distribution;
        if (r.distribution) distribution = serializeDistribution(b, *r.distribution);
        flatbuffers::Offset<quantra::StructuredNoteTimingDiagnostics> timing;
        if (r.timing) timing = serializeTiming(b, *r.timing);
        flatbuffers::Offset<quantra::StructuredNoteGreeks> greeks;
        if (r.greeks) greeks = serializeGreeks(b, *r.greeks);
        flatbuffers::Offset<quantra::StructuredNoteVanillaRepricing> repricing;
        if (r.vanillaRepricing) repricing = serializeVanillaRepricing(b, *r.vanillaRepricing);

        quantra::StructuredNoteResultBuilder rb(b);
        rb.add_trade_id(tradeId);
        rb.add_npv(r.npv);
        rb.add_npv_pct_of_notional(r.npvPctOfNotional);
        if (r.standardError) rb.add_standard_error(*r.standardError);
        rb.add_paths(r.paths);
        rb.add_seed(static_cast<uint64_t>(r.seed));
        rb.add_status(fromStatus(r.status));
        if (r.legs) rb.add_legs(legs);
        rb.add_event_probabilities(probsVec);
        if (r.pBarrierBreach) rb.add_p_barrier_breach(*r.pBarrierBreach);
        if (r.expectedLifeYears) rb.add_expected_life_years(*r.expectedLifeYears);
        if (r.greeks) rb.add_greeks(greeks);
        rb.add_terms(terms);
        rb.add_seasoning(seasoning);
        rb.add_model(model);
        if (r.market) rb.add_market(market);
        if (r.decomposition) rb.add_decomposition(decomposition);
        if (r.checks) rb.add_checks(checks);
        if (r.distribution) rb.add_distribution(distribution);
        if (r.timing) rb.add_timing(timing);
        if (r.vanillaRepricing) rb.add_vanilla_repricing(repricing);
        rows.push_back(rb.Finish());
    }
    auto rowsVec = b.CreateVector(rows);
    quantra::PriceStructuredNoteResponseBuilder rb(b);
    rb.add_notes(rowsVec);
    return rb.Finish();
}

} // namespace quantra
