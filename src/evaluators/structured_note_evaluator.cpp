#include "structured_note_evaluator.h"

#include "basket_model.h"
#include "date_convert.h"
#include "error.h"
#include "path_engine.h"
#include "quanto_builder.h"
#include "time_grid.h"

#include <ql/math/matrixutilities/symmetricschurdecomposition.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/zerospreadedtermstructure.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <set>

namespace quantra {

namespace {

using QuantLib::Date;
using QuantLib::Handle;
using QuantLib::Real;
using QuantLib::Size;

constexpr double kBasisPoint = 1.0e-4;
/// Smallest eigenvalue accepted as positive semi-definite (the same tolerance
/// as the correlation registry's parse-time check).
constexpr double kPsdTolerance = 1.0e-10;

// ---------------------------------------------------------------------------
// The payoff walk (shared by the seasoning and the simulation)
// ---------------------------------------------------------------------------

struct WalkState {
    double memory = 0.0;
    bool called = false;
    bool knockedIn = false;
};

/// What one event did on one path, as fractions of the notional.
struct EventOutcome {
    std::optional<bool> called;
    std::optional<bool> couponEarned;
    /// Paid on the event's payment date (memory included).
    double couponPaid = 0.0;
    /// Notional returned on the event's payment date (a call): 0 or 1.
    double redemptionPaid = 0.0;
    /// Final event only.
    bool breached = false;
    double finalRedemption = 0.0;
    /// <= 0, paid with the final redemption.
    double finalLoss = 0.0;
    double finalUpside = 0.0;
};

EventOutcome stepEvent(
    const ObservationEventData& ev,
    bool isFinal,
    double perf,
    const StructuredNoteTrade& trade,
    WalkState& st) {
    EventOutcome o;
    if (ev.accrualOnly()) {
        st.memory += ev.couponOrZero();
        return o;
    }
    // An `is_final` event (maturity-only observation) has no coupon test and
    // no call test: the memory stays unpaid and only the capital block below
    // runs (the mapper guarantees it is the last event).
    if (!ev.isFinal) {
        const bool callNow = ev.autocallTrigger.has_value() && perf >= *ev.autocallTrigger;
        // Coupon test: its own trigger, or (autocall only) the call test itself.
        const bool earned = ev.couponTrigger ? perf >= *ev.couponTrigger : callNow;
        o.couponEarned = earned;
        if (earned) {
            const double amount = ev.couponOrZero() + st.memory;
            st.memory = 0.0;
            if (callNow || isFinal || !trade.couponRule.payOnlyOnCall) {
                o.couponPaid += amount;
            } else {
                st.memory = amount; // held until the next call or the final event
            }
        } else if (trade.couponRule.memory) {
            st.memory += ev.couponOrZero();
        }
        if (ev.autocallTrigger) {
            o.called = callNow;
            if (callNow) {
                o.couponPaid += st.memory; // everything held
                st.memory = 0.0;
                o.redemptionPaid = 1.0;
                st.called = true;
                return o;
            }
        }
    }
    if (isFinal) {
        const CapitalProtectionData& cap = trade.capital;
        const bool breach = cap.monitoring == CapitalBarrierMonitoring::European
                                ? perf < cap.barrier
                                : st.knockedIn;
        o.breached = breach;
        o.finalRedemption = 1.0;
        if (breach) {
            o.finalLoss = -cap.participation * std::max(cap.putStrike - perf, 0.0);
        }
        if (trade.upside) {
            double u = trade.upside->participation * std::max(perf - trade.upside->strike, 0.0);
            if (trade.upside->cap) u = std::min(u, *trade.upside->cap);
            o.finalUpside = u;
        }
    }
    return o;
}

double basketPerformance(BasketPerformanceRule rule, const std::vector<double>& perfs) {
    switch (rule) {
        case BasketPerformanceRule::WorstOf: return *std::min_element(perfs.begin(), perfs.end());
        case BasketPerformanceRule::BestOf: return *std::max_element(perfs.begin(), perfs.end());
        case BasketPerformanceRule::Average: {
            double s = 0.0;
            for (double p : perfs)
                s += p;
            return s / static_cast<double>(perfs.size());
        }
    }
    return perfs.front();
}

const char* ruleName(BasketPerformanceRule rule) {
    switch (rule) {
        case BasketPerformanceRule::WorstOf: return "WorstOf";
        case BasketPerformanceRule::BestOf: return "BestOf";
        case BasketPerformanceRule::Average: return "Average";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Market resolution
// ---------------------------------------------------------------------------

struct ResolvedUnderlying {
    std::string currency;
    Handle<QuantLib::Quote> spot;
    Handle<QuantLib::YieldTermStructure> dividend; // plain or quanto-adjusted
    /// The underlying's own dividend curve (market diagnostics).
    Handle<QuantLib::YieldTermStructure> plainDividend;
    /// Rate curve of the underlying's own currency: the quanto block's
    /// curve, or the note curve for a same-currency underlying.
    Handle<QuantLib::YieldTermStructure> localCurve;
    Handle<QuantLib::BlackVolTermStructure> vol;
    /// The registry entry the vol came from (its retained inputs rebuild the
    /// shifted vol of the vega bump).
    const BlackVolEntry* volEntry = nullptr;
    quantra::enums::VolSurfaceShape volShape = quantra::enums::VolSurfaceShape_Constant;
    bool exactStep = true;
    // Quanto pieces (for the drift echo and the bumped rebuilds)
    Handle<QuantLib::BlackVolTermStructure> fxVol;
    double fxSpot = 0.0;
    double rho = 0.0;
};

/// The funding-spreaded discount curve of the note: ZeroSpreadedTermStructure
/// (curve, spread, Continuous, Annual, Actual/365 (Fixed)), extrapolating.
std::shared_ptr<QuantLib::YieldTermStructure> spreadedCurve(
    const Handle<QuantLib::YieldTermStructure>& curve, double spread) {
    auto ts = std::make_shared<QuantLib::ZeroSpreadedTermStructure>(
        curve, Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spread)),
        QuantLib::Continuous, QuantLib::Annual, QuantLib::Actual365Fixed());
    ts->enableExtrapolation();
    return ts;
}

/// Every off-diagonal entry of `c` shifted by `delta` and clipped to [-1, 1];
/// when the result is not positive semi-definite (smallest eigenvalue below
/// -kPsdTolerance) it is repaired by eigenvalue clipping: V diag(max(lambda,
/// 0)) V^T rescaled to unit diagonal (`repaired` says so). The fbs doc of
/// StructuredNoteGreeks is the contract; the Python reference reproduces the
/// arithmetic step by step.
QuantLib::Matrix bumpCorrelation(const QuantLib::Matrix& c, double delta, bool& repaired) {
    const Size n = c.rows();
    QuantLib::Matrix m(c);
    for (Size i = 0; i < n; ++i) {
        for (Size j = 0; j < n; ++j) {
            if (i != j) m[i][j] = std::min(1.0, std::max(-1.0, c[i][j] + delta));
        }
    }
    repaired = false;
    const QuantLib::SymmetricSchurDecomposition schur(m);
    const QuantLib::Array& ev = schur.eigenvalues();
    double smallest = ev[0];
    for (Size l = 1; l < n; ++l)
        smallest = std::min(smallest, ev[l]);
    if (smallest >= -kPsdTolerance) return m;
    repaired = true;
    const QuantLib::Matrix& v = schur.eigenvectors();
    QuantLib::Matrix r(n, n, 0.0);
    for (Size i = 0; i < n; ++i) {
        for (Size j = 0; j < n; ++j) {
            double sum = 0.0;
            for (Size l = 0; l < n; ++l)
                sum += v[i][l] * std::max(ev[l], 0.0) * v[j][l];
            r[i][j] = sum;
        }
    }
    QuantLib::Matrix out(n, n, 1.0);
    for (Size i = 0; i < n; ++i) {
        for (Size j = 0; j < n; ++j) {
            if (i != j) out[i][j] = r[i][j] / std::sqrt(r[i][i] * r[j][j]);
        }
    }
    return out;
}

/// Mean and standard error of per-sample differences `d` (fractions of the
/// notional), the path engine's estimator, scaled by `notional`.
NoteSensitivity differenceStatistics(const std::vector<double>& d, double notional) {
    const auto n = static_cast<double>(d.size());
    double sum = 0.0, sumSq = 0.0;
    for (double x : d) {
        sum += x;
        sumSq += x * x;
    }
    const double mean = sum / n;
    NoteSensitivity out;
    out.value = mean * notional;
    if (d.size() < 2) {
        out.standardError = std::numeric_limits<double>::quiet_NaN();
        return out;
    }
    double variance = (sumSq - n * mean * mean) / (n - 1.0);
    if (variance < 0.0) variance = 0.0;
    out.standardError = std::sqrt(variance / n) * notional;
    return out;
}

enum class Bucket { Bond, Coupon, Loss };

/// A cash flow the past already fixed (fraction of notional).
struct KnownFlow {
    Date date;
    double amount;
    Bucket bucket;
};

Handle<QuantLib::YieldTermStructure> findCurve(
    const PricingRegistry& reg, const std::string& id, const std::string& what) {
    auto it = reg.rates.curves.find(id);
    if (it == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND(what + " not found: " + id);
    }
    return Handle<QuantLib::YieldTermStructure>(it->second->currentLink());
}

std::vector<ResolvedUnderlying> resolveUnderlyings(
    const StructuredNoteTrade& trade,
    const PricingRegistry& reg,
    const Handle<QuantLib::YieldTermStructure>& noteCurve) {
    std::vector<ResolvedUnderlying> out;
    out.reserve(trade.underlyings.size());
    for (std::size_t k = 0; k < trade.underlyings.size(); ++k) {
        const BasketUnderlyingData& u = trade.underlyings[k];
        const std::string path = trade.path + ".underlyings[" + std::to_string(k) + "]";
        auto eqIt = reg.equity.equityUnderlyings.find(u.underlyingId);
        if (eqIt == reg.equity.equityUnderlyings.end()) {
            QUANTRA_NOT_FOUND("Equity underlying not found: " + u.underlyingId);
        }
        auto volIt = reg.volatility.blackVols.find(u.volatilityId);
        if (volIt == reg.volatility.blackVols.end()) {
            QUANTRA_NOT_FOUND("Black vol not found: " + u.volatilityId);
        }
        ResolvedUnderlying r;
        r.currency = eqIt->second.currency;
        if (r.currency.empty()) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".underlying_id '" + u.underlyingId +
                "': pricing.equity.equity_underlyings[].currency is required for a structured "
                "note underlying (it decides whether the underlying is quanto)");
        }
        r.spot = eqIt->second.spot;
        r.plainDividend = eqIt->second.dividend;
        r.localCurve = noteCurve;
        r.vol = volIt->second.handle;
        r.volEntry = &volIt->second;
        r.volShape = volIt->second.shapeFb;
        r.exactStep = mc::IsExactStepCapable(r.vol);
        if (r.currency != trade.currency && !u.quanto) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".quanto: required, the underlying '" + u.underlyingId + "' is in " +
                r.currency + " and the note pays in " + trade.currency);
        }
        if (r.currency == trade.currency && u.quanto) {
            QUANTRA_INVALID_ARGUMENT(
                path + ".quanto: must be absent, the underlying '" + u.underlyingId +
                "' is in the note currency " + trade.currency);
        }
        if (!u.quanto) {
            r.dividend = eqIt->second.dividend;
        } else {
            const QuantoData& q = *u.quanto;
            auto localCurve = findCurve(reg, q.discountingCurveId, "Discounting curve");
            const FxSpot& fx = reg.fx.spots.get(q.fxSpotId); // 404 when absent
            const bool pairOk =
                (fx.baseCurrency == r.currency && fx.quoteCurrency == trade.currency) ||
                (fx.baseCurrency == trade.currency && fx.quoteCurrency == r.currency);
            if (!pairOk) {
                QUANTRA_INVALID_ARGUMENT(
                    path + ".quanto.fx_spot_id '" + q.fxSpotId + "' (" + fx.baseCurrency + "/" +
                    fx.quoteCurrency + ") does not quote the underlying currency " + r.currency +
                    " against the note currency " + trade.currency);
            }
            auto fxVolIt = reg.volatility.blackVols.find(q.fxVolatilityId);
            if (fxVolIt == reg.volatility.blackVols.end()) {
                QUANTRA_NOT_FOUND("Black vol not found: " + q.fxVolatilityId);
            }
            if (!mc::IsExactStepCapable(fxVolIt->second.handle)) {
                QUANTRA_INVALID_ARGUMENT(
                    path + ".quanto.fx_volatility_id '" + q.fxVolatilityId +
                    "' must be a Constant or term (AtmMatrix2D) BlackVolSpec: the FX rate is not "
                    "simulated, its vol only enters the quanto drift");
            }
            r.fxVol = fxVolIt->second.handle;
            r.fxSpot = fx.rate->value();
            r.rho = q.fxUnderlyingCorrelation;
            r.localCurve = localCurve;
            r.dividend = buildQuantoDividendCurve(
                eqIt->second.dividend, localCurve, noteCurve, r.vol, r.fxVol, r.fxSpot, r.rho,
                u.initialLevel);
        }
        out.push_back(r);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Per-note pricing
// ---------------------------------------------------------------------------

/// A simulated (future) event with everything the per-path walk needs.
struct FutureEvent {
    std::size_t eventIndex = 0;
    bool accrualOnly = false;
    bool isFinal = false;
    Size gridIndex = 0;
    double dfPayment = 0.0;
    double tObservation = 0.0;
    /// Position among the simulated dated (observed) events; accrual-only
    /// events have none.
    std::size_t observedIndex = 0;
};

/// Wall-clock stage timer for the `timing` block.
class StageClock {
public:
    using clock = std::chrono::steady_clock;
    StageClock() : start_(clock::now()), last_(start_) {}
    /// Close the stage that started at the previous mark (or at construction).
    void mark(const std::string& stage) {
        const auto now = clock::now();
        stages_.push_back({stage, ms(last_, now)});
        last_ = now;
    }
    /// Start a new stage now (dropping the untimed gap since the last mark).
    void reset() { last_ = clock::now(); }
    NoteTimingDiagnostics finish() const {
        NoteTimingDiagnostics t;
        t.stages = stages_;
        t.totalMilliseconds = ms(start_, clock::now());
        return t;
    }

private:
    static double ms(clock::time_point a, clock::time_point b) {
        return std::chrono::duration<double, std::milli>(b - a).count();
    }
    clock::time_point start_, last_;
    std::vector<NoteTimingStage> stages_;
};

/// Per-path records kept for the `distribution` block (individual paths,
/// antithetic ones included).
struct PathRecords {
    std::size_t nObserved = 0;
    std::size_t next = 0;
    /// [observed event][path].
    std::vector<std::vector<double>> perf;
    std::vector<double> payout;
    std::vector<double> payoutPv;
    /// Event index of the call, or SIZE_MAX when the path reached the final.
    std::vector<std::size_t> calledEvent;
    std::vector<unsigned char> breached;

    PathRecords(std::size_t observed, std::size_t paths) : nObserved(observed) {
        perf.assign(observed, std::vector<double>(paths, 0.0));
        payout.assign(paths, 0.0);
        payoutPv.assign(paths, 0.0);
        calledEvent.assign(paths, std::numeric_limits<std::size_t>::max());
        breached.assign(paths, 0);
    }
};

/// Linear interpolation between order statistics on ascending `sorted`
/// (h = (n - 1) p; R / NumPy default), see the fbs doc.
double quantileSorted(const std::vector<double>& sorted, double p) {
    const std::size_t n = sorted.size();
    if (n == 0) return std::numeric_limits<double>::quiet_NaN();
    const double h = static_cast<double>(n - 1) * p;
    const std::size_t lo = static_cast<std::size_t>(std::floor(h));
    const std::size_t hi = std::min(lo + 1, n - 1);
    return sorted[lo] + (h - static_cast<double>(lo)) * (sorted[hi] - sorted[lo]);
}

NoteTermsEcho echoTerms(
    const StructuredNoteTrade& t, const std::vector<ResolvedUnderlying>& und, const Date& asOf) {
    NoteTermsEcho e;
    e.tradeId = t.tradeId;
    e.currency = t.currency;
    e.notional = t.notional;
    e.performanceRule = t.performanceRule;
    for (std::size_t k = 0; k < t.underlyings.size(); ++k) {
        const auto& u = t.underlyings[k];
        NoteTermsUnderlying tu;
        tu.underlyingId = u.underlyingId;
        tu.currency = und[k].currency;
        tu.initialLevel = u.initialLevel;
        tu.volatilityId = u.volatilityId;
        tu.quanto = u.quanto.has_value();
        if (u.quanto) {
            tu.fxSpotId = u.quanto->fxSpotId;
            tu.fxVolatilityId = u.quanto->fxVolatilityId;
            tu.fxUnderlyingCorrelation = u.quanto->fxUnderlyingCorrelation;
        }
        e.underlyings.push_back(tu);
    }
    for (std::size_t i = 0; i < t.events.size(); ++i) {
        const auto& ev = t.events[i];
        NoteTermsEvent te;
        te.index = static_cast<unsigned int>(i);
        te.observationDate = DateToIso(ev.observationDate);
        if (ev.paymentDate) te.paymentDate = DateToIso(*ev.paymentDate);
        te.autocallTrigger = ev.autocallTrigger;
        te.couponTrigger = ev.couponTrigger;
        te.coupon = ev.coupon;
        te.accrualOnly = ev.accrualOnly();
        te.final = (i + 1 == t.events.size());
        te.isFinal = ev.isFinal;
        te.resolvedFromHistory = ev.observationDate <= asOf;
        e.events.push_back(te);
    }
    e.couponMemory = t.couponRule.memory;
    e.couponPayOnlyOnCall = t.couponRule.payOnlyOnCall;
    e.capitalBarrier = t.capital.barrier;
    e.capitalMonitoring = t.capital.monitoring;
    e.putStrike = t.capital.putStrike;
    e.participation = t.capital.participation;
    e.maturityPaymentDate = DateToIso(t.maturityPaymentDate);
    e.discountingCurveId = t.discountingCurveId;
    e.fundingSpreadBp = t.fundingSpreadBp;
    e.correlationId = t.correlationId;
    e.correlationMarkup = t.correlationMarkup;
    e.paths = t.monteCarlo.paths;
    e.seed = t.monteCarlo.seed;
    e.sequence = t.monteCarlo.sequence;
    e.antithetic = t.monteCarlo.antithetic;
    e.brownianBridge = t.monteCarlo.brownianBridge;
    e.stepsPerYear = t.monteCarlo.stepsPerYear;
    return e;
}

/// The level of underlying k on a past observation date, from its
/// historical levels (400 naming the date and the underlying when absent).
double historicalLevel(
    const StructuredNoteTrade& t, std::size_t k, const Date& date, std::size_t eventIndex) {
    const auto& u = t.underlyings[k];
    for (const auto& h : u.historicalLevels) {
        if (h.date == date) return h.value;
    }
    QUANTRA_INVALID_ARGUMENT(
        t.path + ".underlyings[" + std::to_string(k) + "].historical_levels: no level of '" +
        u.underlyingId + "' for the past observation date " + DateToIso(date) + " (events[" +
        std::to_string(eventIndex) + "])");
    return 0.0;
}

/// The correlation matrix of the basket in the note's underlying order: the
/// registry's sub-matrix for two or more underlyings (labels must be exactly
/// the basket), the 1 x 1 unit matrix for one (`correlation_id` must then be
/// absent).
QuantLib::Matrix basketCorrelation(
    const StructuredNoteTrade& trade,
    const PricingRegistry& reg,
    const std::vector<std::string>& ids) {
    if (ids.size() == 1) {
        if (!trade.correlationId.empty()) {
            QUANTRA_INVALID_ARGUMENT(
                trade.path +
                ".correlation_id: must be absent with a single underlying (a 1 x 1 "
                "unit matrix is used; a correlation matrix lists at least two labels)");
        }
        return QuantLib::Matrix(1, 1, 1.0);
    }
    if (trade.correlationId.empty()) {
        QUANTRA_INVALID_ARGUMENT(
            trade.path + ".correlation_id: required with " + std::to_string(ids.size()) +
            " underlyings");
    }
    const CorrelationMatrixRuntime& cm = reg.correlations.get(trade.correlationId); // 404
    std::set<std::string> labels(cm.labels.begin(), cm.labels.end());
    std::set<std::string> basket(ids.begin(), ids.end());
    if (labels != basket) {
        std::string l, b;
        for (const auto& s : cm.labels)
            l += (l.empty() ? "" : ", ") + s;
        for (const auto& s : ids)
            b += (b.empty() ? "" : ", ") + s;
        QUANTRA_INVALID_ARGUMENT(
            trade.path + ".correlation_id '" + trade.correlationId + "': labels [" + l +
            "] must be exactly the note's underlying ids [" + b + "]");
    }
    return reg.correlations.subMatrix(trade.correlationId, ids);
}

/// Discounting of one run: the funding-spreaded discount factor of every
/// future event's payment date (0 for an accrual-only event), of the
/// maturity payment date, and the PV of the flows the past already fixed on
/// that curve (fractions of the notional, by bucket).
struct Discounting {
    std::vector<double> dfPayment;
    double dfMaturity = 0.0;
    double knownNpv = 0.0, knownBond = 0.0, knownCoupon = 0.0, knownLoss = 0.0;
};

NoteResultRow priceNote(
    const StructuredNoteTrade& trade,
    const PricingRegistry& reg,
    const PricingContext& ctx,
    const NoteDiagnosticsRequest& diag,
    const std::optional<NoteGreeksRequest>& greeks) {
    StageClock clock;
    const Date asOf = ctx.asOf;
    const QuantLib::DayCounter timeAxis = QuantLib::Actual365Fixed();
    const std::size_t nU = trade.underlyings.size();
    const std::size_t nE = trade.events.size();

    // --- as-of dependent request rules (400s) ---------------------------------
    for (std::size_t k = 0; k < nU; ++k) {
        const auto& hl = trade.underlyings[k].historicalLevels;
        for (std::size_t j = 0; j < hl.size(); ++j) {
            if (hl[j].date > asOf) {
                QUANTRA_INVALID_ARGUMENT(
                    trade.path + ".underlyings[" + std::to_string(k) + "].historical_levels[" +
                    std::to_string(j) + "].date: " + DateToIso(hl[j].date) +
                    " is after the valuation date " + DateToIso(asOf));
            }
        }
    }
    bool anyPast = false;
    for (const auto& ev : trade.events)
        anyPast = anyPast || ev.observationDate <= asOf;
    const bool dailyClose = trade.capital.monitoring == CapitalBarrierMonitoring::DailyClose;
    if (dailyClose && anyPast && !trade.capital.knockedIn) {
        QUANTRA_INVALID_ARGUMENT(
            trade.path +
            ".capital.knocked_in: required when monitoring is DailyClose and an "
            "event is on or before the valuation date " +
            DateToIso(asOf) + " (the engine cannot infer the past of a daily barrier)");
    }
    if (trade.capital.knockedIn && !(dailyClose && anyPast)) {
        QUANTRA_INVALID_ARGUMENT(
            trade.path + ".capital.knocked_in: must be absent unless monitoring is DailyClose and "
                         "an event is on or before the valuation date");
    }
    if (diag.distribution && diag.samplePaths > trade.monteCarlo.paths) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceStructuredNoteRequest.diagnostics.sample_paths: " +
            std::to_string(diag.samplePaths) + " exceeds " + trade.path + ".monte_carlo.paths (" +
            std::to_string(trade.monteCarlo.paths) + ")");
    }

    // --- market -----------------------------------------------------------------
    const Handle<QuantLib::YieldTermStructure> noteCurve =
        findCurve(reg, trade.discountingCurveId, "Discounting curve");
    const std::shared_ptr<QuantLib::YieldTermStructure> spreaded =
        spreadedCurve(noteCurve, trade.fundingSpreadBp * kBasisPoint);
    auto dfOn = [&](const std::shared_ptr<QuantLib::YieldTermStructure>& curve, const Date& d) {
        return d <= asOf ? 1.0 : curve->discount(d, true);
    };
    auto df = [&](const Date& d) { return dfOn(spreaded, d); };

    const std::vector<ResolvedUnderlying> und = resolveUnderlyings(trade, reg, noteCurve);

    std::vector<std::string> ids;
    for (const auto& u : trade.underlyings)
        ids.push_back(u.underlyingId);
    // The matrix as supplied, then the desk's uniform mark-up on every
    // off-diagonal entry (clipped, repaired like the Greeks' bump when the
    // shift breaks positive semi-definiteness); the simulation, the limit
    // checks and the Greeks' bump all start from `correlation`.
    const QuantLib::Matrix baseCorrelation = basketCorrelation(trade, reg, ids);
    bool markupRepaired = false;
    const QuantLib::Matrix correlation =
        trade.correlationMarkup
            ? bumpCorrelation(baseCorrelation, *trade.correlationMarkup, markupRepaired)
            : baseCorrelation;

    NoteResultRow row;
    row.tradeId = trade.tradeId;
    row.seed = trade.monteCarlo.seed;
    row.terms = echoTerms(trade, und, asOf);
    row.terms.correlationMarkupRepaired = markupRepaired;
    for (std::size_t k = 0; k < nU; ++k) {
        NoteModelUnderlying mu;
        mu.underlyingId = trade.underlyings[k].underlyingId;
        mu.volShape = und[k].volShape;
        mu.localVol = !und[k].exactStep;
        row.model.underlyings.push_back(mu);
    }

    // --- seasoning: resolve the past through the same walk ----------------------
    WalkState state;
    state.knockedIn = trade.capital.knockedIn.value_or(false);
    std::vector<KnownFlow> known;
    NoteSeasoning& seas = row.seasoning;
    seas.asOfDate = DateToIso(asOf);
    std::size_t firstFuture = 0;
    for (std::size_t i = 0; i < nE; ++i) {
        const ObservationEventData& ev = trade.events[i];
        if (ev.observationDate > asOf) break;
        firstFuture = i + 1;
        const bool isFinal = (i + 1 == nE);
        NoteResolvedEvent re;
        re.index = static_cast<unsigned int>(i);
        re.observationDate = DateToIso(ev.observationDate);
        re.accrualOnly = ev.accrualOnly();
        double perf = 0.0;
        if (!ev.accrualOnly()) {
            std::vector<double> perfs(nU);
            for (std::size_t k = 0; k < nU; ++k) {
                const double level = historicalLevel(trade, k, ev.observationDate, i);
                re.levels.push_back(level);
                perfs[k] = level / trade.underlyings[k].initialLevel;
                re.performances.push_back(perfs[k]);
            }
            perf = basketPerformance(trade.performanceRule, perfs);
            re.basketPerformance = perf;
            if (dailyClose && perf < trade.capital.barrier) state.knockedIn = true;
        }
        const EventOutcome o = stepEvent(ev, isFinal, perf, trade, state);
        re.called = o.called;
        re.couponEarned = o.couponEarned;
        re.couponPaid = o.couponPaid;
        re.memoryAfter = state.memory;
        seas.events.push_back(re);
        if (o.couponPaid > 0.0) known.push_back({*ev.paymentDate, o.couponPaid, Bucket::Coupon});
        if (state.called) {
            known.push_back({*ev.paymentDate, o.redemptionPaid, Bucket::Bond});
            seas.status = StructuredNoteStatus::Called;
            break;
        }
        if (isFinal) {
            known.push_back(
                {trade.maturityPaymentDate, o.finalRedemption + o.finalUpside, Bucket::Bond});
            if (o.finalLoss != 0.0) {
                known.push_back({trade.maturityPaymentDate, o.finalLoss, Bucket::Loss});
            }
            seas.status = StructuredNoteStatus::Matured;
        }
    }
    seas.memoryCarried = state.memory;
    row.status = seas.status;

    // PV of what the past fixed (dated after as_of), on a given curve.
    auto knownOn = [&](const std::shared_ptr<QuantLib::YieldTermStructure>& curve, Discounting& d) {
        d.knownNpv = d.knownBond = d.knownCoupon = d.knownLoss = 0.0;
        for (const auto& f : known) {
            if (f.date <= asOf) continue;
            const double pv = f.amount * dfOn(curve, f.date);
            d.knownNpv += pv;
            if (f.bucket == Bucket::Bond) d.knownBond += pv;
            if (f.bucket == Bucket::Coupon) d.knownCoupon += pv;
            if (f.bucket == Bucket::Loss) d.knownLoss += pv;
        }
    };
    double knownUndiscounted = 0.0;
    for (const auto& f : known) {
        if (f.date > asOf) knownUndiscounted += f.amount;
    }
    Discounting base;
    knownOn(spreaded, base);
    const double knownNpv = base.knownNpv;

    if (greeks && seas.status != StructuredNoteStatus::Alive) {
        QUANTRA_INVALID_ARGUMENT(
            trade.path + ": no Greeks for a note that is not alive (status " +
            (seas.status == StructuredNoteStatus::Called ? "Called" : "Matured") +
            ": the past already decided it, there is nothing to bump); omit `greeks`");
    }

    if (seas.status != StructuredNoteStatus::Alive) {
        double total = 0.0;
        Date last = Date();
        for (const auto& f : known) {
            total += f.amount;
            if (f.date > last) last = f.date;
        }
        seas.knownPayment = total;
        seas.knownPaymentDate = DateToIso(last);
        row.npv = knownNpv * trade.notional;
        row.npvPctOfNotional = knownNpv * 100.0;
        row.paths = 0;
        return row;
    }
    seas.nextEventIndex = static_cast<unsigned int>(firstFuture);

    // --- simulation set-up --------------------------------------------------------
    std::vector<FutureEvent> future;
    std::vector<Date> mandatory;
    std::size_t nObserved = 0;
    for (std::size_t i = firstFuture; i < nE; ++i) {
        FutureEvent fe;
        fe.eventIndex = i;
        fe.accrualOnly = trade.events[i].accrualOnly();
        fe.isFinal = (i + 1 == nE);
        if (!fe.accrualOnly) {
            mandatory.push_back(trade.events[i].observationDate);
            fe.tObservation = timeAxis.yearFraction(asOf, trade.events[i].observationDate);
            fe.observedIndex = nObserved++;
        }
        future.push_back(fe);
    }
    // Discount factors of one run on a given (funding-spreaded) curve.
    auto discountingOn = [&](const std::shared_ptr<QuantLib::YieldTermStructure>& curve) {
        Discounting d;
        d.dfPayment.assign(future.size(), 0.0);
        for (std::size_t e = 0; e < future.size(); ++e) {
            if (!future[e].accrualOnly)
                d.dfPayment[e] = dfOn(curve, *trade.events[future[e].eventIndex].paymentDate);
        }
        d.dfMaturity = dfOn(curve, trade.maturityPaymentDate);
        knownOn(curve, d);
        return d;
    };
    base = discountingOn(spreaded);
    const double dfMaturity = base.dfMaturity;

    bool allExact = true;
    std::string surfaceIds;
    for (std::size_t k = 0; k < nU; ++k) {
        if (!und[k].exactStep) {
            allExact = false;
            surfaceIds +=
                (surfaceIds.empty() ? "'" : ", '") + trade.underlyings[k].volatilityId + "'";
        }
    }
    const bool needsSteps = !allExact || dailyClose;
    if (needsSteps && !trade.monteCarlo.stepsPerYear) {
        QUANTRA_INVALID_ARGUMENT(
            trade.path + ".monte_carlo.steps_per_year: required, " +
            (allExact ? std::string("the capital barrier is monitored DailyClose")
                      : "volatility " + surfaceIds +
                            (dailyClose ? " is a strike x expiry surface (local-vol evolution) "
                                          "and the capital barrier is monitored DailyClose"
                                        : " is a strike x expiry surface (local-vol evolution)")));
    }
    if (!needsSteps && trade.monteCarlo.stepsPerYear) {
        QUANTRA_INVALID_ARGUMENT(
            trade.path + ".monte_carlo.steps_per_year: must be absent, every volatility is "
                         "constant or term (exact lognormal steps between observation dates) and "
                         "the capital barrier is European");
    }
    std::optional<Size> steps;
    if (trade.monteCarlo.stepsPerYear) steps = static_cast<Size>(*trade.monteCarlo.stepsPerYear);
    const mc::SimulationGrid grid = mc::BuildSimulationGrid(asOf, mandatory, steps);
    for (auto& fe : future) {
        if (!fe.accrualOnly)
            fe.gridIndex = grid.indexOf(trade.events[fe.eventIndex].observationDate);
    }

    // One Dupire local vol per distinct market for the whole note: every
    // process array built below (price, correlation limits, repricing, the
    // bumped runs whose inputs did not move) shares it.
    mc::LocalVolCache localVols;
    std::vector<mc::BasketAsset> assets;
    for (std::size_t k = 0; k < nU; ++k) {
        assets.push_back(
            mc::MakeBasketAsset(
                trade.underlyings[k].underlyingId, und[k].spot, und[k].dividend, noteCurve,
                und[k].vol));
        localVols.attach(assets.back());
    }
    auto processes = mc::BasketModel::build(assets, correlation);

    // Model echo: quanto drift at the first simulated observation date.
    const Date firstObs = mandatory.front();
    for (std::size_t k = 0; k < nU; ++k) {
        if (!trade.underlyings[k].quanto) continue;
        const double sS = und[k].vol->blackVol(firstObs, trade.underlyings[k].initialLevel, true);
        const double sFx = und[k].fxVol->blackVol(firstObs, und[k].fxSpot, true);
        row.model.underlyings[k].quantoDrift = -und[k].rho * sS * sFx;
    }
    row.model.observationDatesOnly = !steps.has_value();
    row.model.timeSteps = static_cast<unsigned int>(grid.size() - 1);
    for (Size j = 0; j < grid.size(); ++j)
        row.model.timeGrid.push_back(grid.grid[j]);
    row.model.sequenceDimension = static_cast<unsigned int>((grid.size() - 1) * nU);
    clock.mark("market_build");

    // --- the per-path walk -----------------------------------------------------------
    enum : std::size_t {
        A_TOTAL = 0,
        A_BOND,
        A_COUPON,
        A_LOSS,
        A_BREACH,
        A_SURVIVE,
        A_LIFE,
        A_LOSS_UNDISCOUNTED, // -finalLoss (>= 0), undiscounted: E[loss | breach]
        A_BASE
    };
    constexpr std::size_t kPerEvent = 4; // p_call, p_coupon, pv coupons, pv redemption
    const std::size_t nFuture = future.size();
    const Size nAcc = A_BASE + kPerEvent * nFuture;

    std::vector<double> initial(nU);
    for (std::size_t k = 0; k < nU; ++k)
        initial[k] = trade.underlyings[k].initialLevel;
    const BasketPerformanceRule rule = trade.performanceRule;
    const double barrier = trade.capital.barrier;
    const WalkState seed = state;

    auto perfAt = [&](const QuantLib::MultiPath& path, Size j) {
        double agg = 0.0;
        for (std::size_t k = 0; k < nU; ++k) {
            const double p = path[k][j] / initial[k];
            if (k == 0) {
                agg = p;
            } else if (rule == BasketPerformanceRule::WorstOf) {
                agg = std::min(agg, p);
            } else if (rule == BasketPerformanceRule::BestOf) {
                agg = std::max(agg, p);
            } else {
                agg += p;
            }
        }
        return rule == BasketPerformanceRule::Average ? agg / static_cast<double>(nU) : agg;
    };

    // One path through the state machine, discounted on `disc`. `rec`
    // (optional) receives the path's summary at slot `recordAt`: the basket
    // performance at every observed event is recorded whether or not the
    // note is still alive.
    auto walkPath = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc,
                        const Discounting& disc, PathRecords* rec, std::size_t recordAt,
                        const std::vector<FutureEvent>& fut) {
        WalkState st = seed;
        Size prev = 0;
        double payout = knownUndiscounted;
        bool alive = true;
        for (std::size_t e = 0; e < nFuture; ++e) {
            const FutureEvent& fe = fut[e];
            const ObservationEventData& ev = trade.events[fe.eventIndex];
            if (fe.accrualOnly) {
                if (alive) stepEvent(ev, false, 0.0, trade, st);
                continue;
            }
            const double perf = perfAt(path, fe.gridIndex);
            if (rec) rec->perf[fe.observedIndex][recordAt] = perf;
            if (!alive) continue;
            if (dailyClose && !st.knockedIn) {
                for (Size j = prev + 1; j <= fe.gridIndex; ++j) {
                    if (perfAt(path, j) < barrier) {
                        st.knockedIn = true;
                        break;
                    }
                }
            }
            prev = fe.gridIndex;
            const EventOutcome o = stepEvent(ev, fe.isFinal, perf, trade, st);
            const std::size_t b = A_BASE + kPerEvent * e;
            if (o.couponEarned && *o.couponEarned) acc[b + 1] += 1.0;
            if (o.couponPaid > 0.0) {
                const double pv = o.couponPaid * disc.dfPayment[e];
                acc[A_COUPON] += pv;
                acc[A_TOTAL] += pv;
                acc[b + 2] += pv;
                payout += o.couponPaid;
            }
            if (o.called && *o.called) {
                acc[b] += 1.0;
                const double pv = o.redemptionPaid * disc.dfPayment[e];
                acc[A_BOND] += pv;
                acc[A_TOTAL] += pv;
                acc[b + 3] += pv;
                acc[A_LIFE] += fe.tObservation;
                payout += o.redemptionPaid;
                if (rec) rec->calledEvent[recordAt] = fe.eventIndex;
                alive = false;
                if (!rec) break;
                continue;
            }
            if (fe.isFinal) {
                acc[A_SURVIVE] += 1.0;
                if (o.breached) acc[A_BREACH] += 1.0;
                const double bond = (o.finalRedemption + o.finalUpside) * disc.dfMaturity;
                const double loss = o.finalLoss * disc.dfMaturity;
                acc[A_BOND] += bond;
                acc[A_LOSS] += loss;
                acc[A_TOTAL] += bond + loss;
                acc[b + 3] += bond;
                acc[A_LIFE] += fe.tObservation;
                acc[A_LOSS_UNDISCOUNTED] += -o.finalLoss;
                payout += o.finalRedemption + o.finalUpside + o.finalLoss;
                if (rec) rec->breached[recordAt] = o.breached ? 1 : 0;
            }
        }
        if (rec) {
            rec->payout[recordAt] = payout;
            rec->payoutPv[recordAt] = acc[A_TOTAL] + disc.knownNpv;
        }
    };

    mc::PathEngineConfig cfg;
    cfg.paths = trade.monteCarlo.paths;
    cfg.seed = trade.monteCarlo.seed;
    cfg.sequence = trade.monteCarlo.sequence == RandomSequenceKind::Sobol
                       ? mc::PathEngineConfig::Sequence::Sobol
                       : mc::PathEngineConfig::Sequence::PseudoRandom;
    cfg.antithetic = trade.monteCarlo.antithetic;
    cfg.brownianBridge = trade.monteCarlo.brownianBridge;

    // One run on the stream over `timeGrid` with the events indexed on it
    // (`fut`): `totals` (optional) receives every simulated path's acc[TOTAL]
    // in simulation order (the common-random-number differences of the
    // Greeks are formed per sample from them); `extra` (optional) fills the
    // accumulators beyond nAcc from the same path (the vanilla repricing).
    using ExtraPayoff = std::function<void(const QuantLib::MultiPath&, std::vector<Real>&)>;
    auto runPathsOn = [&](const std::shared_ptr<QuantLib::StochasticProcessArray>& procs,
                          const Discounting& disc, PathRecords* rec, std::vector<double>* totals,
                          const std::vector<FutureEvent>& fut, const QuantLib::TimeGrid& timeGrid,
                          Size nAccumulators, const ExtraPayoff* extra) {
        std::size_t call = 0;
        mc::PathFunctor functor = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
            const std::size_t slot = rec ? rec->next++ : 0;
            walkPath(path, acc, disc, rec, slot, fut);
            if (extra) (*extra)(path, acc);
            if (totals) (*totals)[call] = acc[A_TOTAL];
            ++call;
        };
        try {
            return mc::RunPaths(procs, timeGrid, cfg, nAccumulators, functor);
        } catch (const QuantraError&) {
            throw;
        } catch (const std::exception& e) {
            if (allExact) throw;
            // A Dupire local vol derived from a strike x expiry surface. Strike
            // extrapolation is constant (the edge vol), so a simulated level far
            // outside the strike grid is fine; what QuantLib's LocalVolSurface
            // still refuses is a surface that is not arbitrage-free: a negative
            // local variance from the strike curvature (butterfly) or total
            // variance decreasing between two expiries (calendar). The fix is on
            // the surface, so it is a 400 naming it.
            QUANTRA_INVALID_ARGUMENT(
                trade.path + ".underlyings[].volatility_id " + surfaceIds +
                ": the local volatility derived from the surface could not be evaluated along "
                "the simulated paths (" +
                e.what() +
                "); the surface is not arbitrage-free where the paths went (negative local "
                "variance from the strike curvature, or total variance decreasing in time): "
                "recalibrate it (/calibrate-vol-surface returns arbitrage-free surfaces) or "
                "smooth the quoted vols");
        }
        return mc::PathEngineResult{};
    };
    auto runPaths = [&](const std::shared_ptr<QuantLib::StochasticProcessArray>& procs,
                        const Discounting& disc, PathRecords* rec, std::vector<double>* totals) {
        return runPathsOn(procs, disc, rec, totals, future, grid.grid, nAcc, nullptr);
    };

    std::unique_ptr<PathRecords> records;
    if (diag.distribution) {
        records = std::make_unique<PathRecords>(nObserved, static_cast<std::size_t>(cfg.paths));
    }
    std::vector<double> baseTotals;
    if (greeks) baseTotals.assign(static_cast<std::size_t>(cfg.paths), 0.0);
    const mc::PathEngineResult sim =
        runPaths(processes, base, records.get(), greeks ? &baseTotals : nullptr);
    clock.mark("simulation");

    const double N = trade.notional;
    row.npv = (sim.mean[A_TOTAL] + knownNpv) * N;
    row.npvPctOfNotional = (sim.mean[A_TOTAL] + knownNpv) * 100.0;
    row.standardError = sim.standardError[A_TOTAL] * N;
    row.paths = static_cast<unsigned int>(sim.paths);
    NoteLegs legs;
    legs.bondRedemption = {(sim.mean[A_BOND] + base.knownBond) * N, sim.standardError[A_BOND] * N};
    legs.coupons = {(sim.mean[A_COUPON] + base.knownCoupon) * N, sim.standardError[A_COUPON] * N};
    legs.capitalLoss = {(sim.mean[A_LOSS] + base.knownLoss) * N, sim.standardError[A_LOSS] * N};
    row.legs = legs;
    row.pBarrierBreach = sim.mean[A_BREACH];
    row.expectedLifeYears = sim.mean[A_LIFE];
    for (std::size_t e = 0; e < nFuture; ++e) {
        const FutureEvent& fe = future[e];
        if (fe.accrualOnly) continue;
        const ObservationEventData& ev = trade.events[fe.eventIndex];
        NoteEventProbability p;
        p.index = static_cast<unsigned int>(fe.eventIndex);
        p.observationDate = DateToIso(ev.observationDate);
        const std::size_t b = A_BASE + kPerEvent * e;
        if (ev.autocallTrigger) {
            p.pCall = sim.mean[b];
            p.pCallStandardError = sim.standardError[b];
        }
        if (ev.hasCouponTest()) {
            p.pCoupon = sim.mean[b + 1];
            p.pCouponStandardError = sim.standardError[b + 1];
        }
        row.eventProbabilities.push_back(p);
    }

    // --- diagnostics ----------------------------------------------------------------------
    // Forward of underlying k at grid point j: spot x D_div(t) / D_note(t) on
    // the simulation's time axis (what the process drifts along).
    auto forwardAt = [&](std::size_t k, Size j) {
        const double t = grid.grid[j];
        return und[k].spot->value() * und[k].dividend->discount(t, true) /
               noteCurve->discount(t, true);
    };

    if (diag.market) {
        NoteMarketDiagnostics m;
        std::vector<Size> obsGrid;
        std::vector<double> obsTimes;
        for (const auto& fe : future) {
            if (fe.accrualOnly) continue;
            m.observationDates.push_back(DateToIso(trade.events[fe.eventIndex].observationDate));
            obsGrid.push_back(fe.gridIndex);
            obsTimes.push_back(grid.grid[fe.gridIndex]);
        }
        const double tLast = obsTimes.back();
        for (std::size_t k = 0; k < nU; ++k) {
            NoteMarketUnderlying mu;
            mu.underlyingId = trade.underlyings[k].underlyingId;
            mu.spot = und[k].spot->value();
            double tPrev = 0.0;
            for (std::size_t j = 0; j < obsGrid.size(); ++j) {
                const double t = obsTimes[j];
                const double fwd = forwardAt(k, obsGrid[j]);
                mu.forwards.push_back(fwd);
                const double dt = t - tPrev;
                const auto impliedRate = [&](const Handle<QuantLib::YieldTermStructure>& c) {
                    return -std::log(c->discount(t, true) / c->discount(tPrev, true)) / dt;
                };
                mu.impliedDividendYields.push_back(impliedRate(und[k].plainDividend));
                mu.impliedRates.push_back(impliedRate(und[k].localCurve));
                mu.atmVols.push_back(und[k].vol->blackVol(t, fwd, true));
                mu.barrierVols.push_back(und[k].vol->blackVol(t, barrier * initial[k], true));
                tPrev = t;
            }
            if (trade.underlyings[k].quanto) {
                const double sS = und[k].vol->blackVol(tLast, initial[k], true);
                const double sFx = und[k].fxVol->blackVol(tLast, und[k].fxSpot, true);
                mu.quantoDriftBp = -und[k].rho * sS * sFx / kBasisPoint;
            }
            m.underlyings.push_back(mu);
        }
        std::vector<Date> payDates;
        for (const auto& fe : future) {
            if (!fe.accrualOnly) payDates.push_back(*trade.events[fe.eventIndex].paymentDate);
        }
        if (payDates.empty() || payDates.back() != trade.maturityPaymentDate) {
            payDates.push_back(trade.maturityPaymentDate);
        }
        for (const Date& d : payDates) {
            m.paymentDates.push_back(DateToIso(d));
            m.discountFactors.push_back(noteCurve->discount(d, true));
            m.discountFactorsWithFunding.push_back(df(d));
        }
        m.correlationLabels = ids;
        for (Size i = 0; i < correlation.rows(); ++i)
            for (Size j = 0; j < correlation.columns(); ++j)
                m.correlation.push_back(correlation[i][j]);
        if (trade.correlationMarkup) {
            m.correlationMarkup = *trade.correlationMarkup;
            for (Size i = 0; i < baseCorrelation.rows(); ++i)
                for (Size j = 0; j < baseCorrelation.columns(); ++j)
                    m.correlationBase.push_back(baseCorrelation[i][j]);
        }
        row.market = m;
    }

    if (diag.decomposition) {
        NoteDecompositionDiagnostics d;
        for (std::size_t e = 0; e < nFuture; ++e) {
            const FutureEvent& fe = future[e];
            if (fe.accrualOnly) continue;
            const ObservationEventData& ev = trade.events[fe.eventIndex];
            NoteEventDecomposition ed;
            ed.index = static_cast<unsigned int>(fe.eventIndex);
            ed.observationDate = DateToIso(ev.observationDate);
            const std::size_t b = A_BASE + kPerEvent * e;
            if (ev.autocallTrigger) ed.pCall = sim.mean[b];
            if (ev.hasCouponTest()) ed.pCoupon = sim.mean[b + 1];
            ed.pvCoupons = sim.mean[b + 2] * N;
            ed.pvRedemption = sim.mean[b + 3] * N;
            d.events.push_back(ed);
        }
        d.pSurviveToMaturity = sim.mean[A_SURVIVE];
        d.pBreach = sim.mean[A_BREACH];
        d.expectedLifeYears = sim.mean[A_LIFE];
        if (sim.mean[A_BREACH] > 0.0) {
            d.expectedLossGivenBreach = sim.mean[A_LOSS_UNDISCOUNTED] / sim.mean[A_BREACH];
        }
        row.decomposition = d;
    }

    if (diag.checks) {
        NoteChecksDiagnostics c;
        const unsigned int perSample = cfg.antithetic ? 2u : 1u;
        for (const auto& rm : sim.runningMeans) {
            NoteRunningMean r;
            r.fraction = rm.fraction;
            r.paths = static_cast<unsigned int>(rm.samples) * perSample;
            r.npv = (rm.means[A_TOTAL] + knownNpv) * N;
            r.standardError = rm.standardErrors[A_TOTAL] * N;
            c.runningMeans.push_back(r);
        }
        c.runningMeans.push_back(
            {1.0, static_cast<unsigned int>(sim.paths), row.npv, *row.standardError});

        // Deterministic forward walk: every underlying at its forward on
        // every grid point, one path through the same state machine.
        clock.reset();
        {
            std::vector<QuantLib::Path> paths;
            for (std::size_t k = 0; k < nU; ++k) {
                QuantLib::Path p(grid.grid);
                for (Size j = 0; j < grid.size(); ++j)
                    p[j] = forwardAt(k, j);
                paths.push_back(p);
            }
            QuantLib::MultiPath forwardPath(paths);
            std::vector<Real> acc(nAcc, 0.0);
            walkPath(forwardPath, acc, base, nullptr, 0, future);
            c.deterministicForwardNpv = (acc[A_TOTAL] + knownNpv) * N;
        }
        clock.mark("deterministic_forward");
        c.bondOnlyNpv = N * dfMaturity;

        if (nU == 1) {
            c.correlationOneNpv = row.npv;
            c.correlationOneStandardError = *row.standardError;
            c.correlationZeroNpv = row.npv;
            c.correlationZeroStandardError = *row.standardError;
        } else {
            QuantLib::Matrix ones(nU, nU, 1.0);
            QuantLib::Matrix identity(nU, nU, 0.0);
            for (std::size_t k = 0; k < nU; ++k)
                identity[k][k] = 1.0;
            ctx.budget.check();
            clock.reset();
            const mc::PathEngineResult one =
                runPaths(mc::BasketModel::build(assets, ones), base, nullptr, nullptr);
            c.correlationOneNpv = (one.mean[A_TOTAL] + knownNpv) * N;
            c.correlationOneStandardError = one.standardError[A_TOTAL] * N;
            clock.mark("correlation_one");
            ctx.budget.check();
            const mc::PathEngineResult zero =
                runPaths(mc::BasketModel::build(assets, identity), base, nullptr, nullptr);
            c.correlationZeroNpv = (zero.mean[A_TOTAL] + knownNpv) * N;
            c.correlationZeroStandardError = zero.standardError[A_TOTAL] * N;
            clock.mark("correlation_zero");
        }
        row.checks = c;
    }

    if (diag.distribution) {
        clock.reset();
        NoteDistributionDiagnostics d;
        const PathRecords& rec = *records;
        const std::size_t nPaths = rec.payout.size();
        std::vector<double> sorted;
        auto quantiles = [&](const std::vector<double>& values, double& q05, double& q25,
                             double& q50, double& q75, double& q95) {
            sorted = values;
            std::sort(sorted.begin(), sorted.end());
            q05 = quantileSorted(sorted, 0.05);
            q25 = quantileSorted(sorted, 0.25);
            q50 = quantileSorted(sorted, 0.50);
            q75 = quantileSorted(sorted, 0.75);
            q95 = quantileSorted(sorted, 0.95);
        };
        for (const auto& fe : future) {
            if (fe.accrualOnly) continue;
            NoteQuantiles q;
            q.observationDate = DateToIso(trade.events[fe.eventIndex].observationDate);
            quantiles(rec.perf[fe.observedIndex], q.q05, q.q25, q.q50, q.q75, q.q95);
            d.basketPerformanceQuantiles.push_back(q);
        }
        quantiles(rec.payout, d.payoutQ05, d.payoutQ25, d.payoutQ50, d.payoutQ75, d.payoutQ95);
        {
            constexpr std::size_t kBins = 20;
            const double lo = sorted.front(), hi = sorted.back(); // sorted = payouts
            const double width = (hi - lo) / static_cast<double>(kBins);
            d.payoutHistogram.resize(kBins);
            for (std::size_t b = 0; b < kBins; ++b) {
                d.payoutHistogram[b].lower = lo + static_cast<double>(b) * width;
                d.payoutHistogram[b].upper =
                    b + 1 == kBins ? hi : lo + static_cast<double>(b + 1) * width;
            }
            for (double v : rec.payout) {
                std::size_t b = width > 0.0 ? static_cast<std::size_t>((v - lo) / width) : 0;
                if (b >= kBins) b = kBins - 1;
                ++d.payoutHistogram[b].paths;
            }
        }
        for (std::size_t i = 0; i < std::min<std::size_t>(diag.samplePaths, nPaths); ++i) {
            NoteSamplePath sp;
            for (std::size_t j = 0; j < rec.nObserved; ++j)
                sp.basketPerformances.push_back(rec.perf[j][i]);
            sp.payout = rec.payout[i];
            sp.payoutPv = rec.payoutPv[i];
            if (rec.calledEvent[i] != std::numeric_limits<std::size_t>::max()) {
                sp.calledOn = DateToIso(trade.events[rec.calledEvent[i]].observationDate);
            }
            sp.breached = rec.breached[i] != 0;
            d.samplePaths.push_back(sp);
        }
        row.distribution = d;
        clock.mark("diagnostics");
    }

    // --- vanilla repricing: the sheets' options on one extra simulation -------------
    if (diag.vanillaRepricing) {
        ctx.budget.check();
        clock.reset();
        struct RepOption {
            std::size_t k = 0;
            Size gridIndex = 0;
            double strike = 0.0;
            bool isCall = true;
        };
        NoteVanillaRepricing vr;
        std::vector<RepOption> options;
        std::vector<Date> dates = mandatory;
        for (const NoteRepricingSheet& sheet : diag.repricingSheets) {
            std::size_t k = nU;
            for (std::size_t i = 0; i < nU; ++i) {
                if (trade.underlyings[i].underlyingId == sheet.underlyingId) k = i;
            }
            if (k == nU) continue; // another note's underlying
            if (trade.underlyings[k].quanto) {
                QUANTRA_INVALID_ARGUMENT(
                    sheet.path + ".underlying_id '" + sheet.underlyingId +
                    "' is a quanto underlying of " + trade.path +
                    ": its listed options are priced in " + und[k].currency +
                    " under their own measure, the note simulates the quanto-adjusted one; "
                    "the vanilla repricing covers note-currency underlyings only");
            }
            if (sheet.style != OptionSheetStyle::European) {
                QUANTRA_INVALID_ARGUMENT(
                    sheet.path + ".option_style must be European: the repriced options are "
                                 "European payoffs on the simulated paths");
            }
            NoteRepricingUnderlying ru;
            ru.underlyingId = sheet.underlyingId;
            ru.sheetId = sheet.sheetId;
            ru.spot = und[k].spot->value();
            std::map<Date, std::vector<const OptionQuoteLine*>> byExpiry;
            for (const auto& q : sheet.quotes)
                byExpiry[q.expiry].push_back(&q);
            std::set<std::string> reasons;
            for (const auto& [expiry, lines] : byExpiry) {
                NoteRepricingExpiry re;
                re.expiry = lines.front()->expiryIso;
                re.time = timeAxis.yearFraction(asOf, expiry);
                if (expiry <= asOf) {
                    for (const auto* q : lines) {
                        re.dropped.push_back(
                            DroppedQuoteData{
                                q->expiryIso, q->strike, "expiry not after as_of_date"});
                    }
                    reasons.insert("expiry not after as_of_date");
                    ru.expiries.push_back(re);
                    continue;
                }
                const double t = re.time;
                re.discountFactor = noteCurve->discount(t, true);
                re.forward = und[k].spot->value() * und[k].dividend->discount(t, true) /
                             noteCurve->discount(t, true);
                const std::vector<OutOfTheMoneyQuote> used = selectOutOfTheMoneyQuotes(
                    lines, diag.repricingFilter, re.expiry, ru.spot, re.forward, re.discountFactor,
                    t, re.dropped);
                re.quotesUsed = static_cast<unsigned int>(used.size());
                for (const auto& d : re.dropped)
                    reasons.insert(d.reason);
                if (!used.empty()) dates.push_back(expiry);
                for (const auto& u : used) {
                    NoteRepricedOption o;
                    o.expiry = re.expiry;
                    o.time = t;
                    o.strike = u.strike;
                    o.isCall = u.isCall;
                    o.bid = u.bid;
                    o.ask = u.ask;
                    o.mid = u.mid;
                    o.marketVol = u.vol;
                    ru.options.push_back(o);
                    options.push_back({k, 0, u.strike, u.isCall});
                }
                ru.expiries.push_back(re);
            }
            if (ru.options.empty()) {
                std::string list;
                for (const auto& r : reasons)
                    list += (list.empty() ? "" : "; ") + r;
                QUANTRA_INVALID_ARGUMENT(
                    sheet.path + ": no usable quote for underlying '" + sheet.underlyingId + "' (" +
                    list + ")");
            }
            vr.underlyings.push_back(std::move(ru));
        }
        if (vr.underlyings.empty()) {
            std::string basket;
            for (const auto& id : ids)
                basket += (basket.empty() ? "" : ", ") + id;
            QUANTRA_INVALID_ARGUMENT(
                "PriceStructuredNoteRequest.diagnostics.vanilla_repricing_sheets: no sheet for "
                "any underlying of " +
                trade.path + " [" + basket + "]");
        }
        // The repricing grid: the note's dates plus every used expiry.
        std::sort(dates.begin(), dates.end());
        const mc::SimulationGrid repGrid = mc::BuildSimulationGrid(asOf, dates, steps);
        std::vector<FutureEvent> repFuture = future;
        for (auto& fe : repFuture) {
            if (!fe.accrualOnly)
                fe.gridIndex = repGrid.indexOf(trade.events[fe.eventIndex].observationDate);
        }
        {
            std::size_t i = 0;
            for (const auto& ru : vr.underlyings) {
                for (const auto& o : ru.options) {
                    options[i].gridIndex = repGrid.indexOf(DateToQL(o.expiry));
                    ++i;
                }
            }
        }
        const Size nOptions = options.size();
        const ExtraPayoff payoffs = [&](const QuantLib::MultiPath& path, std::vector<Real>& acc) {
            for (std::size_t i = 0; i < nOptions; ++i) {
                const RepOption& o = options[i];
                const double level = path[o.k][o.gridIndex];
                acc[nAcc + i] = std::max(o.isCall ? level - o.strike : o.strike - level, 0.0);
            }
        };
        const mc::PathEngineResult rep = runPathsOn(
            mc::BasketModel::build(assets, correlation), base, nullptr, nullptr, repFuture,
            repGrid.grid, nAcc + nOptions, &payoffs);
        vr.paths = static_cast<unsigned int>(rep.paths);
        vr.timeSteps = static_cast<unsigned int>(repGrid.size() - 1);
        vr.noteNpv = (rep.mean[A_TOTAL] + knownNpv) * N;
        vr.noteStandardError = rep.standardError[A_TOTAL] * N;
        std::size_t i = 0;
        for (auto& ru : vr.underlyings) {
            std::map<std::string, double> dfByExpiry, fwdByExpiry;
            for (const auto& re : ru.expiries) {
                dfByExpiry[re.expiry] = re.discountFactor;
                fwdByExpiry[re.expiry] = re.forward;
            }
            for (auto& o : ru.options) {
                const double df = dfByExpiry.at(o.expiry);
                const double fwd = fwdByExpiry.at(o.expiry);
                o.modelPrice = df * rep.mean[nAcc + i];
                o.standardError = df * rep.standardError[nAcc + i];
                o.insideBidAsk = o.modelPrice >= o.bid && o.modelPrice <= o.ask;
                o.priceError = o.modelPrice - o.mid;
                o.modelVol = impliedBlackVol(
                    o.isCall ? QuantLib::Option::Call : QuantLib::Option::Put, o.strike, fwd,
                    o.modelPrice, df, o.time);
                if (o.modelVol && o.marketVol) o.volError = *o.modelVol - *o.marketVol;
                ++ru.count;
                if (o.insideBidAsk) ++ru.insideBidAsk;
                ru.maxAbsPriceError = std::max(ru.maxAbsPriceError, std::fabs(o.priceError));
                ru.maxStandardError = std::max(ru.maxStandardError, o.standardError);
                if (o.volError) {
                    ru.maxAbsVolError =
                        std::max(ru.maxAbsVolError.value_or(0.0), std::fabs(*o.volError));
                }
                ++i;
            }
            vr.count += ru.count;
            vr.insideBidAsk += ru.insideBidAsk;
            vr.maxAbsPriceError = std::max(vr.maxAbsPriceError, ru.maxAbsPriceError);
            vr.maxStandardError = std::max(vr.maxStandardError, ru.maxStandardError);
            if (ru.maxAbsVolError) {
                vr.maxAbsVolError = std::max(vr.maxAbsVolError.value_or(0.0), *ru.maxAbsVolError);
            }
        }
        row.vanillaRepricing = std::move(vr);
        clock.mark("vanilla_repricing");
    }

    // --- Greeks: bump-and-reprice on the same stream ----------------------------------
    if (greeks) {
        const NoteGreeksRequest& g = *greeks;
        NoteGreeks out;
        out.spotBump = g.spotBump;
        out.volBump = g.volBump;
        out.correlationBump = g.correlationBump;
        out.fundingBumpBp = g.fundingBumpBp;
        out.rateBumpBp = g.rateBumpBp;
        const std::size_t samples = sim.samples;
        const bool antithetic = cfg.antithetic;
        // Per-sample value of a run (fraction of the notional): the antithetic
        // pair averaged as the path engine does, plus the run's known PV.
        auto sampleValues = [&](const std::vector<double>& totals, const Discounting& disc) {
            std::vector<double> v(samples);
            for (std::size_t s = 0; s < samples; ++s) {
                v[s] = (antithetic ? 0.5 * (totals[2 * s] + totals[2 * s + 1]) : totals[s]) +
                       disc.knownNpv;
            }
            return v;
        };
        const std::vector<double> baseValues = sampleValues(baseTotals, base);
        // One bumped run: the assets rebuilt with the given spot factor on
        // underlying `spotOf` (nU = none), the vol shifted on `volOf` (nU =
        // none), every process on `riskFree`; a quanto underlying's curve is
        // rebuilt whenever its vol or the note curve moved.
        constexpr std::size_t kNone = std::numeric_limits<std::size_t>::max();
        auto bumpedRun = [&](const std::string& stage, std::size_t spotOf, double spotFactor,
                             std::size_t volOf,
                             const Handle<QuantLib::YieldTermStructure>& riskFree,
                             bool noteCurveMoved, const QuantLib::Matrix& matrix,
                             const Discounting& disc) {
            ctx.budget.check(); // an expired request stops before the next run
            clock.reset();
            std::vector<mc::BasketAsset> bumped;
            for (std::size_t k = 0; k < nU; ++k) {
                Handle<QuantLib::Quote> spot = und[k].spot;
                if (k == spotOf) {
                    spot = Handle<QuantLib::Quote>(
                        std::make_shared<QuantLib::SimpleQuote>(und[k].spot->value() * spotFactor));
                }
                Handle<QuantLib::BlackVolTermStructure> vol = und[k].vol;
                if (k == volOf) vol = shiftedBlackVol(*und[k].volEntry, g.volBump);
                Handle<QuantLib::YieldTermStructure> dividend = und[k].dividend;
                if (trade.underlyings[k].quanto && (k == volOf || noteCurveMoved)) {
                    dividend = buildQuantoDividendCurve(
                        und[k].plainDividend, und[k].localCurve, riskFree, vol, und[k].fxVol,
                        und[k].fxSpot, und[k].rho, initial[k]);
                }
                bumped.push_back(
                    mc::MakeBasketAsset(
                        trade.underlyings[k].underlyingId, spot, dividend, riskFree, vol));
                localVols.attach(bumped.back());
            }
            std::vector<double> totals(static_cast<std::size_t>(cfg.paths), 0.0);
            runPaths(mc::BasketModel::build(bumped, matrix), disc, nullptr, &totals);
            clock.mark(stage);
            return sampleValues(totals, disc);
        };
        auto difference = [&](const std::vector<double>& a, const std::vector<double>& b,
                              double scale) {
            std::vector<double> d(samples);
            for (std::size_t s = 0; s < samples; ++s)
                d[s] = (a[s] - b[s]) * scale;
            return differenceStatistics(d, N);
        };
        for (std::size_t k = 0; k < nU; ++k) {
            const std::string& id = trade.underlyings[k].underlyingId;
            NoteUnderlyingGreeks ug;
            ug.underlyingId = id;
            const std::vector<double> up = bumpedRun(
                "greek_spot_up_" + id, k, 1.0 + g.spotBump, kNone, noteCurve, false, correlation,
                base);
            const std::vector<double> down = bumpedRun(
                "greek_spot_down_" + id, k, 1.0 - g.spotBump, kNone, noteCurve, false, correlation,
                base);
            ug.delta = difference(up, down, 0.01 / (2.0 * g.spotBump));
            {
                const double scale = (0.01 / g.spotBump) * (0.01 / g.spotBump);
                std::vector<double> d(samples);
                for (std::size_t s = 0; s < samples; ++s)
                    d[s] = (up[s] - 2.0 * baseValues[s] + down[s]) * scale;
                ug.gamma = differenceStatistics(d, N);
            }
            const std::vector<double> volUp =
                bumpedRun("greek_vol_" + id, kNone, 1.0, k, noteCurve, false, correlation, base);
            ug.vega = difference(volUp, baseValues, 0.01 / g.volBump);
            out.underlyings.push_back(ug);
        }
        if (nU > 1) {
            bool repaired = false;
            const QuantLib::Matrix bumpedMatrix =
                bumpCorrelation(correlation, g.correlationBump, repaired);
            out.correlationBumpRepaired = repaired;
            for (Size i = 0; i < bumpedMatrix.rows(); ++i)
                for (Size j = 0; j < bumpedMatrix.columns(); ++j)
                    out.correlationBumped.push_back(bumpedMatrix[i][j]);
            const std::vector<double> corrUp = bumpedRun(
                "greek_correlation", kNone, 1.0, kNone, noteCurve, false, bumpedMatrix, base);
            out.correlationSensitivity = difference(corrUp, baseValues, 0.01 / g.correlationBump);
        }
        {
            const Discounting funded = discountingOn(
                spreadedCurve(noteCurve, (trade.fundingSpreadBp + g.fundingBumpBp) * kBasisPoint));
            const std::vector<double> fundUp = bumpedRun(
                "greek_funding", kNone, 1.0, kNone, noteCurve, false, correlation, funded);
            out.fundingDv01 = difference(fundUp, baseValues, 1.0 / g.fundingBumpBp);
        }
        {
            const Handle<QuantLib::YieldTermStructure> shiftedNote(
                spreadedCurve(noteCurve, g.rateBumpBp * kBasisPoint));
            const Discounting shifted =
                discountingOn(spreadedCurve(shiftedNote, trade.fundingSpreadBp * kBasisPoint));
            const std::vector<double> rateUp =
                bumpedRun("greek_rate", kNone, 1.0, kNone, shiftedNote, true, correlation, shifted);
            out.rho = difference(rateUp, baseValues, 1.0 / g.rateBumpBp);
        }
        std::cout << "Structured note " << trade.tradeId << " Greeks (common random numbers):";
        for (const auto& ug : out.underlyings) {
            std::cout << " " << ug.underlyingId << " delta " << ug.delta.value << " gamma "
                      << ug.gamma.value << " vega " << ug.vega.value;
        }
        if (out.correlationSensitivity) {
            std::cout << " correlation " << out.correlationSensitivity->value
                      << (out.correlationBumpRepaired ? " (repaired)" : "");
        }
        std::cout << " funding_dv01 " << out.fundingDv01.value << " rho " << out.rho.value
                  << std::endl;
        row.greeks = out;
    }

    if (diag.timing) row.timing = clock.finish();
    return row;
}

} // namespace

NoteResult StructuredNoteEvaluator::evaluate(
    const StructuredNoteInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    NoteResult result;
    result.notes.reserve(inputs.notes.size());
    for (const auto& note : inputs.notes) {
        ctx.budget.check();
        result.notes.push_back(priceNote(note, reg, ctx, inputs.diagnostics, inputs.greeks));
    }
    return result;
}

} // namespace quantra
