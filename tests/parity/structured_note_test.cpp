// Structured-note evaluator: collapse tests against closed forms (tolerances
// in multiples of the standard error the engine reports, which must be
// finite and > 0), the deterministic snowball arithmetic at zero volatility,
// and the seasoning of a note whose past is supplied as historical levels.
// The registry is built by hand (flat curves, constant vols, one or two
// underlyings), the evaluator is called directly; the wire path (mapper 400s,
// the two example files) is covered by the HTTP contract suite.
//
//  a. one underlying, one coupon event (trigger K, coupon c), no call, no
//     capital risk: npv = DF_pay N (1 + c P(S_T >= K S0)), P from the
//     AnalyticEuropeanEngine cash-or-nothing digital (3 SE);
//  b. capital only (final event, trigger 0, coupon 0), barrier B, put strike
//     1, participation 1: E[(1 - S_T/S0) 1{S_T < B S0}] = put(B S0)/S0 +
//     (1 - B) digitalPut(B S0) (3 SE); bond + coupons + capital_loss = npv;
//  c. two underlyings, rho in {0, 0.7}, worst-of, one event: p_coupon vs the
//     bivariate normal (3 SE);
//  d. zero vol (1e-8), 4 paths, accrual-only + two autocall events with the
//     forward above the trigger: calls at the first observation and pays
//     N (1 + 0.07 + 0.07) (the snowball arithmetic of the factsheets);
//  e. seasoning: a past event resolved as called with the payment still
//     ahead (npv = DF x payment, Called, paths 0), already paid (npv 0), and
//     not called (memory carried = the past coupons, then paid on the next
//     call at zero vol).
//
// StructuredNoteDiagnosticsTest covers the opt-in blocks: the market block
// against the flat curves (forwards = S D_div / D_rf, implied yields, vols,
// discount factors, the 1 x 1 matrix), the decomposition sums, the
// deterministic forward walk against the zero-vol price, the bond-only
// value, the correlation limits of the two-asset worst-of digital (rho = 1
// is min(p_1, p_2) of the single-asset digitals, rho = 0 their product; the
// corr-one price >= the corr-zero price), quantile sanity (monotone, the
// median of the one-asset lognormal), the histogram, the sample paths
// (antithetic pair), the running means and the timing stages; and that no
// block is returned unless asked for.
//
// StructuredNoteGreeksTest covers the bump-and-reprice Greeks on common
// random numbers: delta, gamma and vega of the one-asset digital against the
// AnalyticEuropeanEngine (the exact target is the analytic value at the same
// bumps, which the common-random-number estimate converges to; the analytic
// delta()/gamma()/vega() differ from it by the finite-difference bias, which
// is reported and allowed for); the correlation sensitivity of the two-asset
// worst-of digital against the bivariate normal at rho and rho + bump (a
// worst-of digital gains with correlation); rho and funding_dv01 of a
// bond-like note (unconditional coupons, no capital risk) against the closed
// form sum CF_i DF_i (exp(-b t_i) - 1) / b (zero Monte Carlo error) and the
// analytic -sum t_i CF_i DF_i within the bias; the repair of a bumped
// correlation matrix that is no longer positive semi-definite (a singular 3
// x 3 matrix bumped by +0.01); the timing stages; and the 400 for Greeks on
// a note the past already decided.
#include "date_convert.h"
#include "error.h"
#include "structured_note_evaluator.h"

#include <ql/exercise.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/distributions/bivariatenormaldistribution.hpp>
#include <ql/math/matrixutilities/symmetricschurdecomposition.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/volatility/equityfx/blackvariancesurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <iostream>
#include <memory>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;

constexpr double kNotional = 1000.0;
constexpr double kSpot = 100.0;
constexpr double kRate = 0.03;

struct NoteMarket {
    Date asOf;
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    PricingRegistry reg;
    PricingContext ctx;
    std::shared_ptr<QuantLib::YieldTermStructure> discount;

    explicit NoteMarket(const Date& valuation) : asOf(valuation) {
        QuantLib::Settings::instance().evaluationDate() = asOf;
        ctx.asOf = asOf;
        discount = std::make_shared<QuantLib::FlatForward>(asOf, kRate, dc);
        reg.rates.curves["USD_OIS"] =
            std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>(discount);
    }

    void addUnderlying(
        const std::string& id, double spot, double q, double vol, const std::string& ccy = "USD") {
        auto div = std::make_shared<QuantLib::FlatForward>(asOf, q, dc);
        reg.rates.curves[id + "_DIV"] =
            std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>(div);
        EquityUnderlyingRuntime u;
        u.currency = ccy;
        u.spot = Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot));
        u.dividend = Handle<QuantLib::YieldTermStructure>(div);
        reg.equity.equityUnderlyings[id] = u;
        BlackVolEntry v;
        v.handle = Handle<QuantLib::BlackVolTermStructure>(
            std::make_shared<QuantLib::BlackConstantVol>(asOf, QuantLib::NullCalendar(), vol, dc));
        v.constantVol = vol;
        v.shapeFb = quantra::enums::VolSurfaceShape_Constant;
        v.referenceDate = asOf;
        v.dayCounter = dc;
        reg.volatility.blackVols[id + "_VOL"] = v;
    }

    void addCorrelation(const std::string& id, const std::vector<std::string>& labels, double rho) {
        std::vector<double> values(labels.size() * labels.size(), rho);
        for (std::size_t i = 0; i < labels.size(); ++i)
            values[i * labels.size() + i] = 1.0;
        addCorrelationMatrix(id, labels, values);
    }

    void addCorrelationMatrix(
        const std::string& id,
        const std::vector<std::string>& labels,
        const std::vector<double>& values) {
        CorrelationMatrix m;
        m.id = id;
        m.labels = labels;
        m.n = labels.size();
        m.values = values;
        reg.correlations.put(MakeCorrelationRuntime(m));
    }

    /// Analytic European option on `id` with the spot and the constant vol
    /// replaced (flat curves of the registry).
    QuantLib::VanillaOption optionAt(
        const std::shared_ptr<QuantLib::StrikedTypePayoff>& payoff,
        const Date& expiry,
        const std::string& id,
        double spot,
        double vol) const {
        auto process = std::make_shared<QuantLib::GeneralizedBlackScholesProcess>(
            Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot)),
            reg.equity.equityUnderlyings.at(id).dividend,
            Handle<QuantLib::YieldTermStructure>(discount),
            Handle<QuantLib::BlackVolTermStructure>(std::make_shared<QuantLib::BlackConstantVol>(
                asOf, QuantLib::NullCalendar(), vol, dc)));
        QuantLib::VanillaOption opt(payoff, std::make_shared<QuantLib::EuropeanExercise>(expiry));
        opt.setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
        return opt;
    }

    double df(const Date& d) const { return discount->discount(d); }
    double t(const Date& d) const { return dc.yearFraction(asOf, d); }

    QuantLib::VanillaOption option(
        const std::shared_ptr<QuantLib::StrikedTypePayoff>& payoff,
        const Date& expiry,
        const std::string& id) const {
        auto process = std::make_shared<QuantLib::GeneralizedBlackScholesProcess>(
            reg.equity.equityUnderlyings.at(id).spot, reg.equity.equityUnderlyings.at(id).dividend,
            Handle<QuantLib::YieldTermStructure>(discount),
            reg.volatility.blackVols.at(id + "_VOL").handle);
        QuantLib::VanillaOption opt(payoff, std::make_shared<QuantLib::EuropeanExercise>(expiry));
        opt.setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
        return opt;
    }
};

ObservationEventData event(
    const Date& obs,
    const Date& pay,
    std::optional<double> autocall,
    std::optional<double> couponTrigger,
    std::optional<double> coupon) {
    ObservationEventData e;
    e.observationDate = obs;
    if (autocall || couponTrigger) e.paymentDate = pay;
    e.autocallTrigger = autocall;
    e.couponTrigger = couponTrigger;
    e.coupon = coupon;
    return e;
}

StructuredNoteTrade baseTrade(const std::vector<std::string>& ids, const std::string& corrId) {
    StructuredNoteTrade t;
    t.path = "notes[0]";
    t.tradeId = "TEST";
    t.currency = "USD";
    t.notional = kNotional;
    for (const auto& id : ids) {
        BasketUnderlyingData u;
        u.underlyingId = id;
        u.volatilityId = id + "_VOL";
        u.initialLevel = kSpot;
        t.underlyings.push_back(u);
    }
    t.performanceRule = BasketPerformanceRule::WorstOf;
    t.couponRule.memory = false;
    t.couponRule.payOnlyOnCall = true;
    t.capital.barrier = 1e-9;
    t.capital.monitoring = CapitalBarrierMonitoring::European;
    t.discountingCurveId = "USD_OIS";
    t.correlationId = corrId;
    t.monteCarlo.paths = 1 << 14;
    t.monteCarlo.seed = 42;
    t.monteCarlo.sequence = RandomSequenceKind::PseudoRandom;
    t.monteCarlo.antithetic = true;
    t.monteCarlo.brownianBridge = false;
    return t;
}

NoteDiagnosticsRequest allDiagnostics(unsigned int samplePaths = 3) {
    NoteDiagnosticsRequest d;
    d.market = d.decomposition = d.checks = d.distribution = d.timing = true;
    d.samplePaths = samplePaths;
    return d;
}

NoteResultRow price(
    const NoteMarket& m,
    const StructuredNoteTrade& t,
    NoteDiagnosticsRequest diag = {},
    std::optional<NoteGreeksRequest> greeks = std::nullopt) {
    StructuredNoteInputs in;
    in.notes.push_back(t);
    in.diagnostics = diag;
    in.greeks = greeks;
    auto out = StructuredNoteEvaluator().evaluate(in, m.reg, m.ctx);
    EXPECT_EQ(out.notes.size(), 1u);
    return out.notes.front();
}

void expectWithin(double actual, double expected, double se, double k, const char* what) {
    ASSERT_TRUE(std::isfinite(se)) << what;
    ASSERT_GT(se, 0.0) << what << ": standard error must be > 0";
    const double z = std::fabs(actual - expected) / se;
    std::cout << "  " << what << ": engine " << actual << " vs closed form " << expected << " ("
              << z << " SE, SE = " << se << ")" << std::endl;
    EXPECT_LE(z, k) << what;
}

} // namespace

class StructuredNoteCollapseTest : public ::testing::Test {
protected:
    const Date valuation{21, QuantLib::October, 2026};
    const Date obs1y{21, QuantLib::October, 2027};
    const Date pay1y{28, QuantLib::October, 2027};
};

// a. coupon only -> cash-or-nothing digital
TEST_F(StructuredNoteCollapseTest, CouponOnlyIsACashOrNothingDigital) {
    std::cout << "\n=== Structured note: coupon-only event vs digital ===" << std::endl;
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    const double K = 1.05, c = 0.07;
    auto t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, pay1y, std::nullopt, K, c));
    t.maturityPaymentDate = pay1y;

    auto digital = m.option(
        std::make_shared<QuantLib::CashOrNothingPayoff>(QuantLib::Option::Call, K * kSpot, 1.0),
        obs1y, "SPX");
    const double p = digital.NPV() / m.df(obs1y);
    const double expected = m.df(pay1y) * kNotional * (1.0 + c * p);

    const auto r = price(m, t);
    ASSERT_TRUE(r.standardError.has_value());
    expectWithin(r.npv, expected, *r.standardError, 3.0, "npv");
    ASSERT_EQ(r.eventProbabilities.size(), 1u);
    ASSERT_TRUE(r.eventProbabilities[0].pCoupon.has_value());
    EXPECT_FALSE(r.eventProbabilities[0].pCall.has_value());
    expectWithin(
        *r.eventProbabilities[0].pCoupon, p, r.eventProbabilities[0].pCouponStandardError, 3.0,
        "p_coupon");
    EXPECT_EQ(r.status, StructuredNoteStatus::Alive);
    EXPECT_EQ(r.paths, 1u << 14);
    ASSERT_TRUE(r.legs.has_value());
    EXPECT_NEAR(r.legs->bondRedemption.pv, m.df(pay1y) * kNotional, 1e-9 * kNotional);
    EXPECT_NEAR(r.legs->capitalLoss.pv, 0.0, 1e-12);
    EXPECT_NEAR(*r.pBarrierBreach, 0.0, 1e-12);
    EXPECT_NEAR(*r.expectedLifeYears, m.t(obs1y), 1e-12);
    EXPECT_TRUE(r.model.observationDatesOnly);
    EXPECT_EQ(r.model.timeSteps, 1u);
    EXPECT_EQ(r.model.sequenceDimension, 1u);
    EXPECT_EQ(r.seasoning.events.size(), 0u);
    EXPECT_EQ(*r.seasoning.nextEventIndex, 0u);
}

// b. capital only -> put + digital put identity
TEST_F(StructuredNoteCollapseTest, CapitalOnlyIsAPutPlusADigitalPut) {
    std::cout << "\n=== Structured note: European capital barrier vs put identity ===" << std::endl;
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.40);
    const double B = 0.6;
    auto t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, obs1y, std::nullopt, 0.0, 0.0));
    t.maturityPaymentDate = obs1y;
    t.capital.barrier = B;
    t.capital.putStrike = 1.0;
    t.capital.participation = 1.0;

    auto put = m.option(
        std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Put, B * kSpot), obs1y,
        "SPX");
    auto digitalPut = m.option(
        std::make_shared<QuantLib::CashOrNothingPayoff>(QuantLib::Option::Put, B * kSpot, 1.0),
        obs1y, "SPX");
    const double lossPerUnit = put.NPV() / kSpot + (1.0 - B) * digitalPut.NPV();
    const double expectedLoss = -kNotional * lossPerUnit;
    const double expectedBreach = digitalPut.NPV() / m.df(obs1y);

    const auto r = price(m, t);
    ASSERT_TRUE(r.legs.has_value());
    expectWithin(
        r.legs->capitalLoss.pv, expectedLoss, r.legs->capitalLoss.standardError, 3.0,
        "capital_loss");
    expectWithin(r.npv, m.df(obs1y) * kNotional + expectedLoss, *r.standardError, 3.0, "npv");
    EXPECT_NEAR(
        r.legs->bondRedemption.pv + r.legs->coupons.pv + r.legs->capitalLoss.pv, r.npv,
        1e-9 * kNotional);
    EXPECT_NEAR(r.legs->coupons.pv, 0.0, 1e-12);
    ASSERT_TRUE(r.pBarrierBreach.has_value());
    std::cout << "  p_breach " << *r.pBarrierBreach << " vs " << expectedBreach << std::endl;
    EXPECT_NEAR(*r.pBarrierBreach, expectedBreach, 0.02);
    EXPECT_GT(*r.pBarrierBreach, 0.05);
}

// c. two underlyings worst-of -> bivariate normal
TEST_F(StructuredNoteCollapseTest, WorstOfTwoIsABivariateNormal) {
    for (double rho : {0.0, 0.7}) {
        std::cout << "\n=== Structured note: worst-of two, rho = " << rho << " ===" << std::endl;
        NoteMarket m(valuation);
        const double q1 = 0.01, q2 = 0.02, s1 = 0.2, s2 = 0.3;
        m.addUnderlying("A", kSpot, q1, s1);
        m.addUnderlying("B", kSpot, q2, s2);
        m.addCorrelation("C", {"A", "B"}, rho);
        const double K = 1.0, c = 0.05;
        auto t = baseTrade({"A", "B"}, "C");
        t.events.push_back(event(obs1y, pay1y, std::nullopt, K, c));
        t.maturityPaymentDate = pay1y;

        const double T = m.t(obs1y);
        auto d = [&](double q, double s) {
            return (-std::log(K) + (kRate - q - 0.5 * s * s) * T) / (s * std::sqrt(T));
        };
        QuantLib::BivariateCumulativeNormalDistributionWe04DP phi2(rho);
        const double p = phi2(d(q1, s1), d(q2, s2));

        const auto r = price(m, t);
        ASSERT_EQ(r.eventProbabilities.size(), 1u);
        expectWithin(
            *r.eventProbabilities[0].pCoupon, p, r.eventProbabilities[0].pCouponStandardError, 3.0,
            "p_coupon");
        expectWithin(
            r.legs->coupons.pv, m.df(pay1y) * kNotional * c * p, r.legs->coupons.standardError, 3.0,
            "coupons");
        EXPECT_EQ(r.model.sequenceDimension, 2u);
    }
}

// d. snowball at zero vol
TEST_F(StructuredNoteCollapseTest, SnowballAtZeroVolCallsAtTheFirstObservation) {
    std::cout << "\n=== Structured note: snowball, zero vol ===" << std::endl;
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 1e-8);
    auto t = baseTrade({"SPX"}, "");
    t.couponRule.memory = true;
    const Date accrual{21, QuantLib::December, 2026};
    const Date obs6m{21, QuantLib::April, 2027}, pay6m{28, QuantLib::April, 2027};
    t.events.push_back(event(accrual, Date(), std::nullopt, std::nullopt, 0.07));
    t.events.push_back(event(obs6m, pay6m, 1.0, std::nullopt, 0.07));
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.07));
    t.maturityPaymentDate = pay1y;
    t.monteCarlo.paths = 4;

    const auto r = price(m, t);
    const double expected = m.df(pay6m) * kNotional * 1.14;
    std::cout << "  npv " << r.npv << " vs " << expected << std::endl;
    EXPECT_NEAR(r.npv, expected, 1e-7 * kNotional);
    ASSERT_TRUE(r.standardError.has_value());
    EXPECT_TRUE(std::isfinite(*r.standardError));
    EXPECT_LT(*r.standardError, 1e-6 * kNotional);
    EXPECT_NEAR(r.legs->bondRedemption.pv, m.df(pay6m) * kNotional, 1e-7 * kNotional);
    EXPECT_NEAR(r.legs->coupons.pv, m.df(pay6m) * kNotional * 0.14, 1e-7 * kNotional);
    ASSERT_EQ(r.eventProbabilities.size(), 2u); // the accrual-only event is not observed
    EXPECT_EQ(r.eventProbabilities[0].index, 1u);
    EXPECT_NEAR(*r.eventProbabilities[0].pCall, 1.0, 1e-12);
    EXPECT_NEAR(*r.eventProbabilities[0].pCoupon, 1.0, 1e-12);
    EXPECT_NEAR(*r.eventProbabilities[1].pCall, 0.0, 1e-12);
    EXPECT_NEAR(*r.expectedLifeYears, m.t(obs6m), 1e-12);
    EXPECT_NEAR(*r.pBarrierBreach, 0.0, 1e-12);
    ASSERT_EQ(r.terms.events.size(), 3u);
    EXPECT_TRUE(r.terms.events[0].accrualOnly);
    EXPECT_FALSE(r.terms.events[0].paymentDate.has_value());
    EXPECT_TRUE(r.terms.events[2].final);
    EXPECT_EQ(r.model.timeSteps, 2u);
}

// e. seasoning
TEST_F(StructuredNoteCollapseTest, SeasoningResolvesThePastBeforeSimulating) {
    const Date accrual{21, QuantLib::December, 2026};
    const Date obs6m{21, QuantLib::April, 2027}, pay6m{28, QuantLib::April, 2027};
    auto makeTrade = [&](double pastLevel) {
        auto t = baseTrade({"SPX"}, "");
        t.couponRule.memory = true;
        t.events.push_back(event(accrual, Date(), std::nullopt, std::nullopt, 0.07));
        t.events.push_back(event(obs6m, pay6m, 1.0, std::nullopt, 0.07));
        t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.07));
        t.maturityPaymentDate = pay1y;
        t.monteCarlo.paths = 4;
        t.underlyings[0].historicalLevels.push_back({obs6m, pastLevel});
        return t;
    };
    {
        std::cout << "\n=== Structured note: seasoning, called, payment ahead ===" << std::endl;
        NoteMarket m(Date(23, QuantLib::April, 2027));
        m.addUnderlying("SPX", 105.0, 0.01, 1e-8);
        const auto r = price(m, makeTrade(105.0));
        EXPECT_EQ(r.status, StructuredNoteStatus::Called);
        EXPECT_EQ(r.paths, 0u);
        EXPECT_FALSE(r.standardError.has_value());
        EXPECT_FALSE(r.legs.has_value());
        EXPECT_TRUE(r.eventProbabilities.empty());
        EXPECT_NEAR(r.npv, m.df(pay6m) * kNotional * 1.14, 1e-9 * kNotional);
        EXPECT_NEAR(*r.seasoning.knownPayment, 1.14, 1e-12);
        EXPECT_EQ(*r.seasoning.knownPaymentDate, "2027-04-28");
        EXPECT_FALSE(r.seasoning.nextEventIndex.has_value());
        ASSERT_EQ(r.seasoning.events.size(), 2u);
        EXPECT_TRUE(r.seasoning.events[0].accrualOnly);
        EXPECT_NEAR(r.seasoning.events[0].memoryAfter, 0.07, 1e-12);
        EXPECT_TRUE(*r.seasoning.events[1].called);
        EXPECT_NEAR(r.seasoning.events[1].couponPaid, 0.14, 1e-12);
        EXPECT_NEAR(*r.seasoning.events[1].basketPerformance, 1.05, 1e-12);
        EXPECT_TRUE(r.terms.events[1].resolvedFromHistory);
        EXPECT_FALSE(r.terms.events[2].resolvedFromHistory);
    }
    {
        std::cout << "\n=== Structured note: seasoning, called, already paid ===" << std::endl;
        NoteMarket m(Date(1, QuantLib::May, 2027));
        m.addUnderlying("SPX", 105.0, 0.01, 1e-8);
        const auto r = price(m, makeTrade(105.0));
        EXPECT_EQ(r.status, StructuredNoteStatus::Called);
        EXPECT_NEAR(r.npv, 0.0, 1e-12);
        EXPECT_EQ(r.paths, 0u);
    }
    {
        std::cout << "\n=== Structured note: seasoning, not called, memory carried ==="
                  << std::endl;
        NoteMarket m(Date(23, QuantLib::April, 2027));
        m.addUnderlying("SPX", kSpot, 0.01, 1e-8);
        const auto r = price(m, makeTrade(95.0));
        EXPECT_EQ(r.status, StructuredNoteStatus::Alive);
        EXPECT_NEAR(r.seasoning.memoryCarried, 0.14, 1e-12);
        EXPECT_EQ(*r.seasoning.nextEventIndex, 2u);
        EXPECT_FALSE(*r.seasoning.events[1].called);
        EXPECT_NEAR(r.seasoning.events[1].couponPaid, 0.0, 1e-12);
        EXPECT_EQ(r.paths, 4u);
        // Zero vol from spot 100 at the valuation date: the final call pays
        // 1 + 0.14 (carried) + 0.07 on the final payment date.
        EXPECT_NEAR(r.npv, m.df(pay1y) * kNotional * 1.21, 1e-7 * kNotional);
        EXPECT_NEAR(r.legs->coupons.pv, m.df(pay1y) * kNotional * 0.21, 1e-7 * kNotional);
        ASSERT_EQ(r.eventProbabilities.size(), 1u);
        EXPECT_EQ(r.eventProbabilities[0].index, 2u);
        EXPECT_NEAR(*r.eventProbabilities[0].pCall, 1.0, 1e-12);
    }
}

// `is_final` (maturity-only observation) vs the zero-coupon workaround
// (`coupon_trigger 0.0, coupon 0.0` on the last event): the Scotia shape (one
// 100 % call with a premium, then the final date with the 75 % trigger and the
// 250 % upside, no coupon). Same stream, same walk apart from the always-true
// zero coupon test, so every number is bit-identical; the marker shows in the
// terms echo and the final event carries no coupon probability.
TEST_F(StructuredNoteCollapseTest, FinalMarkerEqualsTheZeroCouponWorkaround) {
    std::cout << "\n=== Structured note: is_final vs coupon_trigger 0.0 / coupon 0.0 ==="
              << std::endl;
    const Date obs2y{20, QuantLib::October, 2028}, pay2y{25, QuantLib::October, 2028};
    NoteMarket m(valuation);
    m.addUnderlying("NDX", kSpot, 0.0043, 0.2246);
    m.addUnderlying("RTY", kSpot, 0.0098, 0.2156);
    m.addCorrelation("NDX_RTY", {"NDX", "RTY"}, 0.747);
    auto makeTrade = [&](bool marker) {
        auto t = baseTrade({"NDX", "RTY"}, "NDX_RTY");
        t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.1475));
        if (marker) {
            ObservationEventData fin;
            fin.observationDate = obs2y;
            fin.paymentDate = pay2y;
            fin.isFinal = true;
            t.events.push_back(fin);
        } else {
            t.events.push_back(event(obs2y, pay2y, std::nullopt, 0.0, 0.0));
        }
        t.maturityPaymentDate = pay2y;
        t.capital.barrier = 0.75;
        t.upside = UpsideData{1.0, 2.5, std::nullopt};
        t.monteCarlo.sequence = RandomSequenceKind::Sobol;
        t.monteCarlo.brownianBridge = true;
        t.monteCarlo.paths = 1 << 12;
        return t;
    };
    const auto a = price(m, makeTrade(false));
    const auto b = price(m, makeTrade(true));
    std::cout << "  workaround " << a.npv << " marker " << b.npv << std::endl;
    EXPECT_EQ(a.npv, b.npv);
    EXPECT_EQ(*a.standardError, *b.standardError);
    ASSERT_TRUE(a.legs && b.legs);
    EXPECT_EQ(a.legs->bondRedemption.pv, b.legs->bondRedemption.pv);
    EXPECT_EQ(a.legs->coupons.pv, b.legs->coupons.pv);
    EXPECT_EQ(a.legs->capitalLoss.pv, b.legs->capitalLoss.pv);
    EXPECT_EQ(*a.pBarrierBreach, *b.pBarrierBreach);
    EXPECT_EQ(*a.expectedLifeYears, *b.expectedLifeYears);
    EXPECT_GT(b.npv, 0.0);
    EXPECT_GT(*b.pBarrierBreach, 0.0);
    // Echo: both last events are `final`; only the marker one is `is_final`.
    ASSERT_EQ(b.terms.events.size(), 2u);
    EXPECT_TRUE(b.terms.events[1].final);
    EXPECT_TRUE(b.terms.events[1].isFinal);
    EXPECT_FALSE(b.terms.events[0].isFinal);
    EXPECT_FALSE(b.terms.events[1].accrualOnly);
    EXPECT_FALSE(a.terms.events[1].isFinal);
    // Probabilities: the workaround's zero coupon test holds on every path
    // that is still alive at the final (p_coupon = 1 - p_call at 1y); the
    // marker has no coupon test, so no p_coupon (and no p_call) at the final.
    ASSERT_EQ(a.eventProbabilities.size(), 2u);
    ASSERT_EQ(b.eventProbabilities.size(), 2u);
    EXPECT_NEAR(*a.eventProbabilities[1].pCoupon, 1.0 - *a.eventProbabilities[0].pCall, 1e-12);
    EXPECT_FALSE(b.eventProbabilities[1].pCoupon.has_value());
    EXPECT_FALSE(b.eventProbabilities[1].pCall.has_value());
    EXPECT_EQ(*a.eventProbabilities[0].pCall, *b.eventProbabilities[0].pCall);

    // Memory is NOT paid at an is_final event (no coupon test): with memory
    // and zero vol from below the trigger, the workaround pays the carried
    // 14.75 % at maturity, the marker pays nothing but the redemption.
    {
        NoteMarket zero(valuation);
        zero.addUnderlying("NDX", 0.9 * kSpot, 0.0, 1e-8);
        zero.addUnderlying("RTY", 0.9 * kSpot, 0.0, 1e-8);
        zero.addCorrelation("NDX_RTY", {"NDX", "RTY"}, 0.747);
        auto w = makeTrade(false), k = makeTrade(true);
        w.couponRule.memory = k.couponRule.memory = true;
        w.monteCarlo.paths = k.monteCarlo.paths = 4;
        const auto rw = price(zero, w), rk = price(zero, k);
        EXPECT_NEAR(rw.legs->coupons.pv, zero.df(pay2y) * kNotional * 0.1475, 1e-7 * kNotional);
        EXPECT_NEAR(rk.legs->coupons.pv, 0.0, 1e-12);
        EXPECT_NEAR(rw.legs->bondRedemption.pv, rk.legs->bondRedemption.pv, 1e-9 * kNotional);
    }

    // Seasoning: an is_final event in the past means the note has matured;
    // the redemption (with the upside) is the known payment.
    {
        NoteMarket past(Date(22, QuantLib::October, 2028));
        past.addUnderlying("NDX", 120.0, 0.0, 0.2);
        past.addUnderlying("RTY", 115.0, 0.0, 0.2);
        past.addCorrelation("NDX_RTY", {"NDX", "RTY"}, 0.747);
        auto k = makeTrade(true);
        for (auto& u : k.underlyings) {
            u.historicalLevels.push_back({obs1y, 0.95 * kSpot}); // no call at 1y
            u.historicalLevels.push_back({obs2y, u.underlyingId == "NDX" ? 120.0 : 115.0});
        }
        const auto r = price(past, k);
        EXPECT_EQ(r.status, StructuredNoteStatus::Matured);
        EXPECT_EQ(r.paths, 0u);
        EXPECT_FALSE(r.standardError.has_value());
        ASSERT_EQ(r.seasoning.events.size(), 2u);
        EXPECT_FALSE(r.seasoning.events[1].called.has_value());
        EXPECT_FALSE(r.seasoning.events[1].couponEarned.has_value());
        EXPECT_NEAR(*r.seasoning.events[1].basketPerformance, 1.15, 1e-12);
        // worst-of 1.15: upside 2.5 x 0.15 = 0.375 on top of the notional.
        EXPECT_NEAR(*r.seasoning.knownPayment, 1.375, 1e-12);
        EXPECT_EQ(*r.seasoning.knownPaymentDate, "2028-10-25");
        EXPECT_NEAR(r.npv, past.df(pay2y) * kNotional * 1.375, 1e-9 * kNotional);
        EXPECT_TRUE(r.terms.events[1].isFinal && r.terms.events[1].resolvedFromHistory);
    }
}

// Rules that need the market: steps_per_year with constant vols, quanto.
TEST_F(StructuredNoteCollapseTest, MarketDependentRulesAre400s) {
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    m.addUnderlying("SMI", kSpot, 0.01, 0.25, "CHF");
    m.addUnderlying("NDX", kSpot, 0.01, 0.25);
    m.addCorrelation("C", {"SPX"}, 0.0);
    m.addCorrelation("C2", {"SPX", "SMI"}, 0.5);
    auto expect400 = [&](const StructuredNoteTrade& t, const char* needle) {
        try {
            price(m, t);
            FAIL() << "expected a 400 containing '" << needle << "'";
        } catch (const QuantraInvalidArgument& e) {
            EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
        }
    };
    auto t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
    t.maturityPaymentDate = pay1y;
    t.monteCarlo.stepsPerYear = 12;
    expect400(t, "notes[0].monte_carlo.steps_per_year: must be absent");

    t = baseTrade({"SPX", "SMI"}, "C2");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
    t.maturityPaymentDate = pay1y;
    expect400(t, "notes[0].underlyings[1].quanto: required");

    t = baseTrade({"SPX"}, "C2");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
    t.maturityPaymentDate = pay1y;
    expect400(t, "notes[0].correlation_id: must be absent with a single underlying");

    t = baseTrade({"SPX", "NDX"}, "");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
    t.maturityPaymentDate = pay1y;
    expect400(t, "notes[0].correlation_id: required with 2 underlyings");

    t = baseTrade({"SPX", "NDX"}, "C");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
    t.maturityPaymentDate = pay1y;
    expect400(t, "notes[0].correlation_id 'C': labels [SPX] must be exactly");

    t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
    t.maturityPaymentDate = pay1y;
    t.underlyings[0].historicalLevels.push_back({pay1y, 100.0});
    expect400(t, "historical_levels[0].date: 2027-10-28 is after the valuation date");
}

// Input guards: vol-shape allow-list, discrete dividends, local-variance scan.
class StructuredNoteGuardsTest : public ::testing::Test {
protected:
    const Date valuation{21, QuantLib::October, 2026};
    const Date obs6m{21, QuantLib::April, 2027};
    const Date pay6m{28, QuantLib::April, 2027};
    const Date obs1y{21, QuantLib::October, 2027};
    const Date pay1y{28, QuantLib::October, 2027};

    /// One SPX underlying whose vol is a strike x expiry surface (flat in
    /// strike, `vol6m` to 6M and `vol1y` to 1Y): the shape is SmileCube3D, so
    /// the note is simulated with Dupire local vol.
    static void useSurface(NoteMarket& m, double vol6m, double vol1y) {
        const Date& asOf = m.asOf;
        const std::vector<Date> expiries = {
            Date(21, QuantLib::April, 2027), Date(21, QuantLib::October, 2027)};
        const std::vector<double> strikes = {60.0, 100.0, 160.0};
        QuantLib::Matrix vols(strikes.size(), expiries.size());
        for (std::size_t i = 0; i < strikes.size(); ++i) {
            vols[i][0] = vol6m;
            vols[i][1] = vol1y;
        }
        auto surface = std::make_shared<QuantLib::BlackVarianceSurface>(
            asOf, QuantLib::NullCalendar(), expiries, strikes, vols, m.dc);
        surface->enableExtrapolation();
        BlackVolEntry& v = m.reg.volatility.blackVols.at("SPX_VOL");
        v.handle = Handle<QuantLib::BlackVolTermStructure>(surface);
        v.shapeFb = quantra::enums::VolSurfaceShape_SmileCube3D;
    }

    StructuredNoteTrade surfaceNote() const {
        auto t = baseTrade({"SPX"}, "");
        t.events.push_back(event(obs6m, pay6m, std::nullopt, 0.0, 0.02));
        t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
        t.maturityPaymentDate = pay1y;
        t.monteCarlo.paths = 1 << 10;
        t.monteCarlo.stepsPerYear = 12;
        return t;
    }

    static void expect400Containing(
        const NoteMarket& m,
        const StructuredNoteTrade& t,
        const std::vector<std::string>& needles) {
        try {
            price(m, t);
            FAIL() << "expected a 400";
        } catch (const QuantraInvalidArgument& e) {
            const std::string what = e.what();
            for (const auto& n : needles)
                EXPECT_NE(what.find(n), std::string::npos) << "'" << n << "' not in: " << what;
        }
    }
};

TEST_F(StructuredNoteGuardsTest, VolShapeOutsideTheAllowListIs400) {
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    // A shape no note can simulate (stands for the SSVI shapes of 0.14.0).
    m.reg.volatility.blackVols.at("SPX_VOL").shapeFb =
        static_cast<quantra::enums::VolSurfaceShape>(99);
    auto t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
    t.maturityPaymentDate = pay1y;
    expect400Containing(
        m, t,
        {"notes[0].underlyings[0].volatility_id 'SPX_VOL' (underlying 'SPX')", "vol shape 99",
         "arrive with the LocalVol kernel in 0.15.0"});
}

TEST_F(StructuredNoteGuardsTest, DiscreteDividendsAre400) {
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    auto& eq = m.reg.equity.equityUnderlyings.at("SPX");
    eq.discreteDividendDates.push_back(Date(15, QuantLib::April, 2027));
    eq.discreteDividendAmounts.push_back(2.5);
    auto t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.05));
    t.maturityPaymentDate = pay1y;
    expect400Containing(
        m, t,
        {"notes[0].underlyings[0].underlying_id 'SPX'", "1 discrete dividend(s)",
         "only through the dividend-yield curve", "fold the cash dividends"});
}

TEST_F(StructuredNoteGuardsTest, ScanRejectsACalendarArbitragedSurfaceNamingTimeAndStrike) {
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    useSurface(m, 0.40, 0.15); // total variance 0.08 at 6M, 0.0225 at 1Y
    expect400Containing(
        m, surfaceNote(),
        {"notes[0].underlyings[0].volatility_id 'SPX_VOL' (underlying 'SPX')", "at t = 0.4986",
         "(2027-04-21), K = ", "decreasing variance", "calendar or butterfly arbitrage"});
}

TEST_F(StructuredNoteGuardsTest, ScanAcceptsAnArbitrageFreeSurface) {
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    useSurface(m, 0.20, 0.25);
    const NoteResultRow r = price(m, surfaceNote());
    EXPECT_TRUE(std::isfinite(r.npv));
    EXPECT_GT(r.npv, 0.0);
}

class StructuredNoteDiagnosticsTest : public ::testing::Test {
protected:
    const Date valuation{21, QuantLib::October, 2026};
    const Date obs6m{21, QuantLib::April, 2027};
    const Date pay6m{28, QuantLib::April, 2027};
    const Date obs1y{21, QuantLib::October, 2027};
    const Date pay1y{28, QuantLib::October, 2027};
};

TEST_F(StructuredNoteDiagnosticsTest, BlocksAreAbsentUnlessRequested) {
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    auto t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, pay1y, std::nullopt, 1.05, 0.07));
    t.maturityPaymentDate = pay1y;
    const auto r = price(m, t);
    EXPECT_FALSE(r.market.has_value());
    EXPECT_FALSE(r.decomposition.has_value());
    EXPECT_FALSE(r.checks.has_value());
    EXPECT_FALSE(r.distribution.has_value());
    EXPECT_FALSE(r.timing.has_value());
    NoteDiagnosticsRequest only;
    only.timing = true;
    const auto r2 = price(m, t, only);
    EXPECT_FALSE(r2.market.has_value());
    EXPECT_FALSE(r2.checks.has_value());
    ASSERT_TRUE(r2.timing.has_value());
    EXPECT_NEAR(r2.npv, r.npv, 1e-12 * kNotional); // same stream, same price
}

// One asset, flat market: every market number is analytic; the single
// asset's limit prices are the headline.
TEST_F(StructuredNoteDiagnosticsTest, MarketBlockMatchesTheFlatCurves) {
    std::cout << "\n=== Structured note diagnostics: market block ===" << std::endl;
    NoteMarket m(valuation);
    const double q = 0.01, sigma = 0.25, B = 0.6;
    m.addUnderlying("SPX", kSpot, q, sigma);
    auto t = baseTrade({"SPX"}, "");
    t.couponRule.memory = true;
    t.events.push_back(event(obs6m, pay6m, 1.0, std::nullopt, 0.03));
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.03));
    t.maturityPaymentDate = Date(4, QuantLib::November, 2027);
    t.capital.barrier = B;
    const auto r = price(m, t, allDiagnostics());
    ASSERT_TRUE(r.market.has_value());
    const NoteMarketDiagnostics& mk = *r.market;
    ASSERT_EQ(mk.observationDates.size(), 2u);
    EXPECT_EQ(mk.observationDates[0], "2027-04-21");
    ASSERT_EQ(mk.underlyings.size(), 1u);
    const NoteMarketUnderlying& u = mk.underlyings[0];
    EXPECT_EQ(u.underlyingId, "SPX");
    EXPECT_NEAR(u.spot, kSpot, 1e-12);
    const auto& div = *m.reg.rates.curves.at("SPX_DIV");
    const Date obs[] = {obs6m, obs1y};
    for (int j = 0; j < 2; ++j) {
        const double T = m.t(obs[j]);
        const double fwd = kSpot * div->discount(T) / m.discount->discount(T);
        EXPECT_NEAR(u.forwards[j], fwd, 1e-10) << j;
        EXPECT_NEAR(u.forwards[j], kSpot * std::exp((kRate - q) * T), 1e-8) << j;
        EXPECT_NEAR(u.impliedDividendYields[j], q, 1e-10) << j;
        EXPECT_NEAR(u.impliedRates[j], kRate, 1e-10) << j;
        EXPECT_NEAR(u.atmVols[j], sigma, 1e-12) << j;
        EXPECT_NEAR(u.barrierVols[j], sigma, 1e-12) << j; // constant vol: strike ignored
    }
    EXPECT_FALSE(u.quantoDriftBp.has_value());
    ASSERT_EQ(mk.paymentDates.size(), 3u); // two event payments + maturity
    EXPECT_EQ(mk.paymentDates[2], "2027-11-04");
    for (std::size_t j = 0; j < 3; ++j) {
        const Date d = DateToQL(mk.paymentDates[j]);
        EXPECT_NEAR(mk.discountFactors[j], m.df(d), 1e-12);
        EXPECT_NEAR(mk.discountFactorsWithFunding[j], m.df(d), 1e-12); // spread 0
    }
    ASSERT_EQ(mk.correlationLabels, std::vector<std::string>{"SPX"});
    ASSERT_EQ(mk.correlation, std::vector<double>{1.0});
    EXPECT_TRUE(r.terms.correlationId.empty());

    ASSERT_TRUE(r.checks.has_value());
    EXPECT_NEAR(r.checks->correlationOneNpv, r.npv, 1e-12);
    EXPECT_NEAR(r.checks->correlationZeroNpv, r.npv, 1e-12);
    EXPECT_NEAR(r.checks->correlationOneStandardError, *r.standardError, 1e-12);
    EXPECT_NEAR(r.checks->bondOnlyNpv, kNotional * m.df(t.maturityPaymentDate), 1e-9);
    ASSERT_TRUE(r.timing.has_value());
    std::vector<std::string> names;
    for (const auto& s : r.timing->stages)
        names.push_back(s.stage);
    EXPECT_EQ(
        names, (std::vector<std::string>{
                   "market_build", "simulation", "deterministic_forward", "diagnostics"}));
    EXPECT_GT(r.timing->totalMilliseconds, 0.0);
}

// Funding spread: the two discount factor vectors differ by exp(-s t).
TEST_F(StructuredNoteDiagnosticsTest, FundingSpreadShowsInTheDiscountFactors) {
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    auto t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.03));
    t.maturityPaymentDate = pay1y;
    t.fundingSpreadBp = 100.0;
    NoteDiagnosticsRequest d;
    d.market = d.checks = true;
    const auto r = price(m, t, d);
    ASSERT_EQ(r.market->paymentDates.size(), 1u);
    const double T = m.t(pay1y);
    EXPECT_NEAR(r.market->discountFactors[0], m.df(pay1y), 1e-12);
    EXPECT_NEAR(r.market->discountFactorsWithFunding[0], m.df(pay1y) * std::exp(-0.01 * T), 1e-12);
    EXPECT_NEAR(r.checks->bondOnlyNpv, kNotional * m.df(pay1y) * std::exp(-0.01 * T), 1e-9);
}

// Zero vol: the deterministic forward walk IS the Monte Carlo price.
TEST_F(StructuredNoteDiagnosticsTest, DeterministicForwardWalkEqualsTheZeroVolPrice) {
    std::cout << "\n=== Structured note diagnostics: deterministic forward walk ===" << std::endl;
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 1e-8);
    auto t = baseTrade({"SPX"}, "");
    t.couponRule.memory = true;
    const Date accrual{21, QuantLib::December, 2026};
    t.events.push_back(event(accrual, Date(), std::nullopt, std::nullopt, 0.07));
    t.events.push_back(event(obs6m, pay6m, 1.0, std::nullopt, 0.07));
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.07));
    t.maturityPaymentDate = pay1y;
    t.monteCarlo.paths = 4;
    const auto r = price(m, t, allDiagnostics(4));
    const double expected = m.df(pay6m) * kNotional * 1.14;
    EXPECT_NEAR(r.npv, expected, 1e-7 * kNotional);
    ASSERT_TRUE(r.checks.has_value());
    std::cout << "  deterministic " << r.checks->deterministicForwardNpv << " vs mc " << r.npv
              << std::endl;
    EXPECT_NEAR(r.checks->deterministicForwardNpv, expected, 1e-9 * kNotional);
    EXPECT_NEAR(r.checks->deterministicForwardNpv, r.npv, 1e-7 * kNotional);
    // 4 paths antithetic = 2 samples: the 1/4 point is empty, 1/2 = 1 sample,
    // 3/4 repeats it, then the full run.
    ASSERT_EQ(r.checks->runningMeans.size(), 2u);
    EXPECT_NEAR(r.checks->runningMeans[0].fraction, 0.5, 1e-12);
    EXPECT_EQ(r.checks->runningMeans[0].paths, 2u);
    EXPECT_NEAR(r.checks->runningMeans[1].fraction, 1.0, 1e-12);
    EXPECT_EQ(r.checks->runningMeans[1].paths, 4u);
    EXPECT_NEAR(r.checks->runningMeans[1].npv, r.npv, 1e-12);
    // Distribution at zero vol: every path pays 1.14 on the first call.
    ASSERT_TRUE(r.distribution.has_value());
    const auto& d = *r.distribution;
    ASSERT_EQ(d.basketPerformanceQuantiles.size(), 2u);
    EXPECT_NEAR(d.payoutQ05, 1.14, 1e-9);
    EXPECT_NEAR(d.payoutQ95, 1.14, 1e-9);
    ASSERT_EQ(d.payoutHistogram.size(), 20u);
    EXPECT_EQ(d.payoutHistogram[0].paths, 4u); // degenerate: all in the first bin
    EXPECT_NEAR(d.payoutHistogram[0].lower, 1.14, 1e-9);
    ASSERT_EQ(d.samplePaths.size(), 4u);
    for (const auto& sp : d.samplePaths) {
        ASSERT_EQ(sp.basketPerformances.size(), 2u);
        EXPECT_NEAR(sp.payout, 1.14, 1e-9);
        EXPECT_NEAR(sp.payoutPv, expected / kNotional, 1e-9);
        ASSERT_TRUE(sp.calledOn.has_value());
        EXPECT_EQ(*sp.calledOn, "2027-04-21");
        EXPECT_FALSE(sp.breached);
    }
    ASSERT_TRUE(r.decomposition.has_value());
    EXPECT_NEAR(r.decomposition->pSurviveToMaturity, 0.0, 1e-12);
    EXPECT_FALSE(r.decomposition->expectedLossGivenBreach.has_value()); // no breach
}

// Two assets, worst-of digital: rho = 1 collapses to min(p_1, p_2), rho = 0
// to p_1 p_2 (independent), and the worst-of is short correlation.
TEST_F(StructuredNoteDiagnosticsTest, CorrelationLimitsOfTheWorstOfDigital) {
    std::cout << "\n=== Structured note diagnostics: correlation limits ===" << std::endl;
    NoteMarket m(valuation);
    const double q1 = 0.01, q2 = 0.02, s1 = 0.2, s2 = 0.3, K = 1.0, c = 0.05;
    m.addUnderlying("A", kSpot, q1, s1);
    m.addUnderlying("B", kSpot, q2, s2);
    m.addCorrelation("C", {"A", "B"}, 0.5);
    auto t = baseTrade({"A", "B"}, "C");
    t.events.push_back(event(obs1y, pay1y, std::nullopt, K, c));
    t.maturityPaymentDate = pay1y;
    auto digitalP = [&](const std::string& id) {
        return m.option(
                    std::make_shared<QuantLib::CashOrNothingPayoff>(
                        QuantLib::Option::Call, K * kSpot, 1.0),
                    obs1y, id)
                   .NPV() /
               m.df(obs1y);
    };
    const double p1 = digitalP("A"), p2 = digitalP("B");
    const auto r = price(m, t, allDiagnostics());
    ASSERT_TRUE(r.checks.has_value());
    const auto& ck = *r.checks;
    const double base = m.df(pay1y) * kNotional;
    std::cout << "  rho=1 " << ck.correlationOneNpv << " +/- " << ck.correlationOneStandardError
              << " vs " << base * (1.0 + c * std::min(p1, p2)) << "; rho=0 "
              << ck.correlationZeroNpv << " +/- " << ck.correlationZeroStandardError << " vs "
              << base * (1.0 + c * p1 * p2) << "; rho=0.5 " << r.npv << std::endl;
    expectWithin(
        ck.correlationOneNpv, base * (1.0 + c * std::min(p1, p2)), ck.correlationOneStandardError,
        3.0, "correlation_one_npv");
    expectWithin(
        ck.correlationZeroNpv, base * (1.0 + c * p1 * p2), ck.correlationZeroStandardError, 3.0,
        "correlation_zero_npv");
    EXPECT_GT(ck.correlationOneNpv, ck.correlationZeroNpv);
    EXPECT_GT(ck.correlationOneNpv, r.npv);
    EXPECT_GT(r.npv, ck.correlationZeroNpv);
    // The deterministic forward walk: both forwards are above K, so the
    // coupon is paid.
    EXPECT_NEAR(ck.deterministicForwardNpv, base * (1.0 + c), 1e-9 * kNotional);
    ASSERT_EQ(r.market->correlation.size(), 4u);
    EXPECT_NEAR(r.market->correlation[1], 0.5, 1e-12);
    std::vector<std::string> names;
    for (const auto& s : r.timing->stages)
        names.push_back(s.stage);
    EXPECT_EQ(
        names, (std::vector<std::string>{
                   "market_build", "simulation", "deterministic_forward", "correlation_one",
                   "correlation_zero", "diagnostics"}));
}

// Decomposition and distribution on the capital-only note: event PVs add
// up to the buckets, E[loss | breach] is the undiscounted loss bucket over
// p_breach, quantiles are monotone with the lognormal median, the
// histogram counts every path, the antithetic pair multiplies to the
// squared median.
TEST_F(StructuredNoteDiagnosticsTest, DecompositionAndDistributionOfTheCapitalOnlyNote) {
    std::cout << "\n=== Structured note diagnostics: decomposition / distribution ===" << std::endl;
    NoteMarket m(valuation);
    const double q = 0.01, sigma = 0.40, B = 0.6, c = 0.02;
    m.addUnderlying("SPX", kSpot, q, sigma);
    auto t = baseTrade({"SPX"}, "");
    t.couponRule.payOnlyOnCall = false;
    t.events.push_back(event(obs6m, pay6m, std::nullopt, 0.0, c)); // unconditional coupon
    t.events.push_back(event(obs1y, pay1y, std::nullopt, 0.0, c));
    t.maturityPaymentDate = pay1y;
    t.capital.barrier = B;
    const auto r = price(m, t, allDiagnostics(2));
    ASSERT_TRUE(r.decomposition.has_value());
    const auto& d = *r.decomposition;
    ASSERT_EQ(d.events.size(), 2u);
    double sumCoupons = 0.0, sumRedemption = 0.0;
    for (const auto& e : d.events) {
        sumCoupons += e.pvCoupons;
        sumRedemption += e.pvRedemption;
        EXPECT_FALSE(e.pCall.has_value());
        EXPECT_NEAR(*e.pCoupon, 1.0, 1e-12);
    }
    EXPECT_NEAR(sumCoupons, r.legs->coupons.pv, 1e-9 * kNotional);
    EXPECT_NEAR(sumRedemption, r.legs->bondRedemption.pv, 1e-9 * kNotional);
    EXPECT_NEAR(d.events[0].pvCoupons, c * m.df(pay6m) * kNotional, 1e-9 * kNotional);
    EXPECT_NEAR(d.pSurviveToMaturity, 1.0, 1e-12);
    EXPECT_NEAR(d.pBreach, *r.pBarrierBreach, 1e-12);
    EXPECT_NEAR(d.expectedLifeYears, m.t(obs1y), 1e-12);
    ASSERT_TRUE(d.expectedLossGivenBreach.has_value());
    // One payment date: the PV bucket undiscounts exactly.
    EXPECT_NEAR(
        *d.expectedLossGivenBreach,
        -r.legs->capitalLoss.pv / m.df(pay1y) / kNotional / *r.pBarrierBreach, 1e-9);
    EXPECT_GT(*d.expectedLossGivenBreach, 1.0 - B); // the loss is at least 1 - B on a breach

    ASSERT_TRUE(r.distribution.has_value());
    const auto& dist = *r.distribution;
    ASSERT_EQ(dist.basketPerformanceQuantiles.size(), 2u);
    for (const auto& qu : dist.basketPerformanceQuantiles) {
        EXPECT_LE(qu.q05, qu.q25);
        EXPECT_LE(qu.q25, qu.q50);
        EXPECT_LE(qu.q50, qu.q75);
        EXPECT_LE(qu.q75, qu.q95);
    }
    const double T = m.t(obs1y);
    const double median = std::exp((kRate - q - 0.5 * sigma * sigma) * T);
    std::cout << "  median at 1y " << dist.basketPerformanceQuantiles[1].q50 << " vs lognormal "
              << median << std::endl;
    EXPECT_NEAR(dist.basketPerformanceQuantiles[1].q50, median, 0.01);
    EXPECT_LE(dist.payoutQ05, dist.payoutQ50);
    EXPECT_LE(dist.payoutQ50, dist.payoutQ95);
    EXPECT_NEAR(dist.payoutQ95, 1.0 + 2 * c, 1e-9); // the barrier holds on most paths
    ASSERT_EQ(dist.payoutHistogram.size(), 20u);
    unsigned int total = 0;
    for (std::size_t b = 0; b < 20; ++b) {
        total += dist.payoutHistogram[b].paths;
        if (b > 0) {
            EXPECT_NEAR(dist.payoutHistogram[b].lower, dist.payoutHistogram[b - 1].upper, 1e-12);
        }
    }
    EXPECT_EQ(total, r.paths);
    EXPECT_NEAR(dist.payoutHistogram[19].upper, 1.0 + 2 * c, 1e-9);
    ASSERT_EQ(dist.samplePaths.size(), 2u);
    // path 1 is the antithetic of path 0: the log-returns are negated, so
    // the product of the performances is the squared median.
    const double p0 = dist.samplePaths[0].basketPerformances[1];
    const double p1 = dist.samplePaths[1].basketPerformances[1];
    EXPECT_NEAR(p0 * p1, median * median, 1e-9);
    for (const auto& sp : dist.samplePaths) {
        EXPECT_FALSE(sp.calledOn.has_value());
        EXPECT_EQ(sp.breached, sp.basketPerformances[1] < B);
        const double expectedPayout =
            2 * c + 1.0 - (sp.breached ? std::max(1.0 - sp.basketPerformances[1], 0.0) : 0.0);
        EXPECT_NEAR(sp.payout, expectedPayout, 1e-12);
    }
    // Running means: every partial estimate is within a few of its own SE.
    ASSERT_EQ(r.checks->runningMeans.size(), 4u);
    for (const auto& rm : r.checks->runningMeans) {
        EXPECT_LE(std::fabs(rm.npv - r.npv), 5.0 * rm.standardError + 1e-9) << rm.fraction;
    }
}

// ---------------------------------------------------------------------------
// Greeks (bump-and-reprice, common random numbers)
// ---------------------------------------------------------------------------

class StructuredNoteGreeksTest : public ::testing::Test {
protected:
    const Date valuation{21, QuantLib::October, 2026};
    const Date obs1y{21, QuantLib::October, 2027};
    const Date pay1y{28, QuantLib::October, 2027};
    const Date obs6m{21, QuantLib::April, 2027};
    const Date pay6m{28, QuantLib::April, 2027};
    const NoteGreeksRequest defaults{};

    /// `actual` against the exact finite-difference target `fd` within 3 SE,
    /// and against the analytic Greek `analytic` within 3 SE plus the
    /// finite-difference bias |fd - analytic| (which is not a Monte Carlo
    /// error).
    static void expectGreek(
        const NoteSensitivity& actual, double fd, double analytic, const char* what) {
        expectWithin(actual.value, fd, actual.standardError, 3.0, what);
        const double bias = std::fabs(fd - analytic);
        std::cout << "  " << what << ": analytic Greek " << analytic << ", finite-difference bias "
                  << bias << std::endl;
        EXPECT_LE(std::fabs(actual.value - analytic), 3.0 * actual.standardError + bias) << what;
    }
};

// Delta, gamma and vega of the one-asset digital coupon.
TEST_F(StructuredNoteGreeksTest, DeltaGammaVegaOfTheDigitalMatchTheAnalyticEngine) {
    std::cout << "\n=== Structured note Greeks: digital coupon vs AnalyticEuropeanEngine ==="
              << std::endl;
    NoteMarket m(valuation);
    const double sigma = 0.25;
    m.addUnderlying("SPX", kSpot, 0.01, sigma);
    const double K = 1.05, c = 0.07;
    auto t = baseTrade({"SPX"}, "");
    t.events.push_back(event(obs1y, pay1y, std::nullopt, K, c));
    t.maturityPaymentDate = pay1y;

    auto payoff =
        std::make_shared<QuantLib::CashOrNothingPayoff>(QuantLib::Option::Call, K * kSpot, 1.0);
    // Note value as a function of spot and vol: DF_pay N (1 + c P(S_T >= K S0)).
    const double scale = m.df(pay1y) * kNotional * c / m.df(obs1y);
    auto value = [&](double spot, double vol) {
        return m.df(pay1y) * kNotional + scale * m.optionAt(payoff, obs1y, "SPX", spot, vol).NPV();
    };
    auto digital = m.optionAt(payoff, obs1y, "SPX", kSpot, sigma);
    const double h = defaults.spotBump, dv = defaults.volBump;
    const double v0 = value(kSpot, sigma);
    const double up = value(kSpot * (1.0 + h), sigma), down = value(kSpot * (1.0 - h), sigma);
    const double fdDelta = (up - down) / (2.0 * h) * 0.01;
    const double fdGamma = (up - 2.0 * v0 + down) / (h * h) * 0.01 * 0.01;
    const double fdVega = (value(kSpot, sigma + dv) - v0) / dv * 0.01;
    const double anDelta = scale * digital.delta() * kSpot * 0.01;
    const double anGamma = scale * digital.gamma() * kSpot * kSpot * 0.01 * 0.01;
    const double anVega = scale * digital.vega() * 0.01;

    const auto r = price(m, t, allDiagnostics(0), defaults);
    ASSERT_TRUE(r.greeks.has_value());
    const NoteGreeks& g = *r.greeks;
    ASSERT_EQ(g.underlyings.size(), 1u);
    EXPECT_EQ(g.underlyings[0].underlyingId, "SPX");
    expectGreek(g.underlyings[0].delta, fdDelta, anDelta, "delta (per 1% spot)");
    expectGreek(g.underlyings[0].gamma, fdGamma, anGamma, "gamma (per 1%^2)");
    expectGreek(g.underlyings[0].vega, fdVega, anVega, "vega (per vol point)");
    EXPECT_GT(g.underlyings[0].delta.value, 0.0);
    // Cash-or-nothing call vega = -DF n(d2) d1 / sigma: negative here (d1 > 0).
    EXPECT_LT(anVega, 0.0);
    EXPECT_LT(g.underlyings[0].vega.value, 0.0);
    // One underlying: no correlation sensitivity, nothing repaired.
    EXPECT_FALSE(g.correlationSensitivity.has_value());
    EXPECT_FALSE(g.correlationBumpRepaired);
    EXPECT_TRUE(g.correlationBumped.empty());
    EXPECT_LT(g.fundingDv01.value, 0.0);
    EXPECT_LT(g.rho.value, 0.0); // discounting dominates the digital's drift gain
    EXPECT_DOUBLE_EQ(g.spotBump, 0.01);
    EXPECT_DOUBLE_EQ(g.volBump, 0.01);
    EXPECT_DOUBLE_EQ(g.correlationBump, 0.01);
    EXPECT_DOUBLE_EQ(g.fundingBumpBp, 1.0);
    EXPECT_DOUBLE_EQ(g.rateBumpBp, 1.0);
    // The price itself is the plain run (same stream).
    expectWithin(r.npv, v0, *r.standardError, 3.0, "npv");
    // Timing: one stage per bumped run, after the diagnostic stages.
    ASSERT_TRUE(r.timing.has_value());
    std::vector<std::string> stages;
    for (const auto& st : r.timing->stages)
        stages.push_back(st.stage);
    const std::vector<std::string> expected = {
        "market_build",  "simulation",        "deterministic_forward",
        "diagnostics",   "greek_spot_up_SPX", "greek_spot_down_SPX",
        "greek_vol_SPX", "greek_funding",     "greek_rate"};
    EXPECT_EQ(stages, expected);
}

// Correlation sensitivity of the two-asset worst-of digital: p(min >= K)
// increases with rho; the exact target is the bivariate normal at rho + bump.
TEST_F(StructuredNoteGreeksTest, WorstOfDigitalGainsWithCorrelation) {
    std::cout << "\n=== Structured note Greeks: worst-of digital correlation sensitivity ==="
              << std::endl;
    NoteMarket m(valuation);
    m.addUnderlying("A", kSpot, 0.01, 0.25);
    m.addUnderlying("B", kSpot, 0.02, 0.30);
    const double rho = 0.5, K = 1.0, c = 0.07;
    m.addCorrelation("CORR", {"A", "B"}, rho);
    auto t = baseTrade({"A", "B"}, "CORR");
    t.events.push_back(event(obs1y, pay1y, std::nullopt, K, c));
    t.maturityPaymentDate = pay1y;

    // P(S_A >= K S0, S_B >= K S0) = M(d_A, d_B; rho) with d = (ln(1/K) + (r - q - s^2/2) T) / (s sqrt T).
    const double T = m.t(obs1y);
    auto d = [&](double q, double s) {
        return (std::log(1.0 / K) + (kRate - q - 0.5 * s * s) * T) / (s * std::sqrt(T));
    };
    auto p = [&](double corr) {
        return QuantLib::BivariateCumulativeNormalDistributionWe04DP(corr)(
            d(0.01, 0.25), d(0.02, 0.30));
    };
    const double bump = defaults.correlationBump;
    const double fd = m.df(pay1y) * kNotional * c * (p(rho + bump) - p(rho)) / bump * 0.01;

    const auto r = price(m, t, {}, defaults);
    ASSERT_TRUE(r.greeks.has_value());
    const NoteGreeks& g = *r.greeks;
    ASSERT_TRUE(g.correlationSensitivity.has_value());
    expectWithin(
        g.correlationSensitivity->value, fd, g.correlationSensitivity->standardError, 3.0,
        "correlation_sensitivity (per 0.01)");
    EXPECT_GT(g.correlationSensitivity->value, 0.0);
    EXPECT_FALSE(g.correlationBumpRepaired);
    const std::vector<double> used = {1.0, rho + bump, rho + bump, 1.0};
    ASSERT_EQ(g.correlationBumped.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i)
        EXPECT_NEAR(g.correlationBumped[i], used[i], 1e-15);
    EXPECT_EQ(g.underlyings.size(), 2u);
    EXPECT_GT(g.underlyings[0].delta.value, 0.0);
    EXPECT_GT(g.underlyings[1].delta.value, 0.0);
    EXPECT_FALSE(r.timing.has_value());
}

// rho and funding_dv01 of a bond-like note (coupons unconditional, no
// capital risk): pure discounting sensitivities, zero Monte Carlo error.
TEST_F(StructuredNoteGreeksTest, RhoAndFundingOfABondLikeNoteAreTheDiscountingSensitivity) {
    std::cout << "\n=== Structured note Greeks: bond-like note rho / funding_dv01 ===" << std::endl;
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    const double c = 0.02;
    auto t = baseTrade({"SPX"}, "");
    t.fundingSpreadBp = 80.0;
    t.couponRule.payOnlyOnCall = false; // each coupon paid on its own date
    t.events.push_back(event(obs6m, pay6m, std::nullopt, 0.0, c));
    t.events.push_back(event(obs1y, pay1y, std::nullopt, 0.0, c));
    t.maturityPaymentDate = pay1y;

    // Closed forms on the funding-spreaded flat curve: DF_f(d) = DF(d) exp(-s t(d)).
    const double s = t.fundingSpreadBp * 1e-4, b = defaults.rateBumpBp * 1e-4;
    auto dff = [&](const Date& d) { return m.df(d) * std::exp(-s * m.t(d)); };
    const std::vector<std::pair<Date, double>> flows = {{pay6m, c}, {pay1y, c}, {pay1y, 1.0}};
    double fd = 0.0, analytic = 0.0, pv = 0.0;
    for (const auto& f : flows) {
        fd += f.second * dff(f.first) * (std::exp(-b * m.t(f.first)) - 1.0);
        analytic += -m.t(f.first) * f.second * dff(f.first);
        pv += f.second * dff(f.first);
    }
    fd *= kNotional / defaults.rateBumpBp;
    analytic *= kNotional * 1e-4;

    const auto r = price(m, t, {}, defaults);
    ASSERT_TRUE(r.greeks.has_value());
    const NoteGreeks& g = *r.greeks;
    EXPECT_NEAR(r.npv, pv * kNotional, 1e-9 * kNotional);
    std::cout << "  rho: engine " << g.rho.value << " (SE " << g.rho.standardError
              << ") vs closed form " << fd << ", analytic -sum t CF DF " << analytic << std::endl;
    EXPECT_NEAR(g.rho.value, fd, 1e-9 * kNotional);
    EXPECT_NEAR(g.rho.value, analytic, 1e-3 * std::fabs(analytic)); // finite-difference bias
    EXPECT_LT(g.rho.standardError, 1e-8 * kNotional);               // constant per-path difference
    // The funding bump shifts the same discounting by the same amount.
    EXPECT_NEAR(g.fundingDv01.value, fd, 1e-9 * kNotional);
    EXPECT_LT(g.fundingDv01.standardError, 1e-8 * kNotional);
    // Nothing depends on the spot or the vol.
    EXPECT_NEAR(g.underlyings[0].delta.value, 0.0, 1e-12);
    EXPECT_NEAR(g.underlyings[0].gamma.value, 0.0, 1e-12);
    EXPECT_NEAR(g.underlyings[0].vega.value, 0.0, 1e-12);
}

// A bumped correlation matrix that is no longer positive semi-definite is
// repaired (eigenvalue clipping, unit diagonal) and the matrix used is
// returned.
TEST_F(StructuredNoteGreeksTest, CorrelationBumpIsRepairedOnASingularMatrix) {
    std::cout << "\n=== Structured note Greeks: correlation bump repair ===" << std::endl;
    NoteMarket m(valuation);
    m.addUnderlying("A", kSpot, 0.01, 0.25);
    m.addUnderlying("B", kSpot, 0.01, 0.25);
    m.addUnderlying("C", kSpot, 0.01, 0.25);
    // det = 1 + 2 (0.9 0.9 0.62) - 0.81 - 0.81 - 0.3844 = 0: singular, so +0.01
    // on every pair makes it indefinite.
    m.addCorrelationMatrix(
        "CORR", {"A", "B", "C"}, {1.0, 0.9, 0.9, 0.9, 1.0, 0.62, 0.9, 0.62, 1.0});
    auto t = baseTrade({"A", "B", "C"}, "CORR");
    t.monteCarlo.paths = 1 << 10;
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.07));
    t.maturityPaymentDate = pay1y;

    const auto r = price(m, t, {}, defaults);
    ASSERT_TRUE(r.greeks.has_value());
    const NoteGreeks& g = *r.greeks;
    EXPECT_TRUE(g.correlationBumpRepaired);
    ASSERT_EQ(g.correlationBumped.size(), 9u);
    QuantLib::Matrix used(3, 3);
    for (std::size_t i = 0; i < 3; ++i) {
        for (std::size_t j = 0; j < 3; ++j) {
            used[i][j] = g.correlationBumped[i * 3 + j];
            EXPECT_NEAR(used[i][j], g.correlationBumped[j * 3 + i], 1e-15) << "symmetric";
        }
        EXPECT_DOUBLE_EQ(used[i][i], 1.0);
    }
    const QuantLib::SymmetricSchurDecomposition schur(used);
    double smallest = schur.eigenvalues()[0];
    for (QuantLib::Size l = 1; l < 3; ++l)
        smallest = std::min(smallest, schur.eigenvalues()[l]);
    std::cout << "  repaired matrix: " << used << "  smallest eigenvalue " << smallest << std::endl;
    EXPECT_GE(smallest, -1e-10);
    // Close to the bumped entries (the repair moves them by a few 1e-3).
    EXPECT_NEAR(used[0][1], 0.91, 0.02);
    EXPECT_NEAR(used[0][2], 0.91, 0.02);
    EXPECT_NEAR(used[1][2], 0.63, 0.02);
    ASSERT_TRUE(g.correlationSensitivity.has_value());
    EXPECT_TRUE(std::isfinite(g.correlationSensitivity->value));
}

// Greeks on a note the past already decided: 400.
TEST_F(StructuredNoteGreeksTest, GreeksOnACalledNoteAre400) {
    NoteMarket m(valuation);
    m.addUnderlying("SPX", kSpot, 0.01, 0.25);
    auto t = baseTrade({"SPX"}, "");
    const Date pastObs{21, QuantLib::April, 2026}, pastPay{28, QuantLib::April, 2026};
    t.underlyings[0].historicalLevels.push_back({pastObs, 1.1 * kSpot});
    t.events.push_back(event(pastObs, pastPay, 1.0, std::nullopt, 0.07));
    t.events.push_back(event(obs1y, pay1y, 1.0, std::nullopt, 0.07));
    t.maturityPaymentDate = pay1y;
    // Without Greeks: a Called note, nothing simulated.
    const auto plain = price(m, t);
    EXPECT_EQ(plain.status, StructuredNoteStatus::Called);
    EXPECT_FALSE(plain.greeks.has_value());
    try {
        price(m, t, {}, defaults);
        FAIL() << "expected a 400";
    } catch (const QuantraInvalidArgument& e) {
        EXPECT_NE(
            std::string(e.what()).find("no Greeks for a note that is not alive"), std::string::npos)
            << e.what();
        EXPECT_NE(std::string(e.what()).find("Called"), std::string::npos) << e.what();
    }
}

}} // namespace quantra::testing
