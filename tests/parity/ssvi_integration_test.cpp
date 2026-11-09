// eSSVI surfaces in the engine's own shapes: the SsviVolSpec surface (makeSsviSurface: the
// reference forward through the slice forwards, no curve), the Monte Carlo core reading its
// closed-form local vol (mc::LocalVolCache, mc::EdgeClampedLocalVol with the strike edges, the
// FlatLocalVol clamp and the floor), the vega bump that keeps it eSSVI, and the Ssvi method of
// /calibrate-vol-surface on an SSVI power-law sheet (recovery, arbitrage report, local vol check,
// grid sampling, dropped quotes).
#include "basket_model.h"
#include "date_convert.h"
#include "error.h"
#include "implied_forwards_evaluator.h"
#include "ssvi.h"
#include "ssvi_vol_parser.h"
#include "ssvi_vol_surface.h"
#include "vol_surface_calibration_evaluator.h"

#include <ql/pricingengines/blackformula.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;

struct PayloadMarket {
    Date ref{21, QuantLib::October, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    double spot = 100.0;
    // The good set of ssvi_test.cpp at 91, 182, 365, 730 days, forwards at 2.5 % carry.
    std::vector<Date> expiries{ref + 91, ref + 182, ref + 365, ref + 730};
    std::vector<SsviSliceParams> params{
        {0.010000, -0.70, 0.10},
        {0.018050, -0.68, 0.14},
        {0.034225, -0.65, 0.20},
        {0.064800, -0.60, 0.28}};
    std::vector<double> forwards() const {
        std::vector<double> f;
        for (const Date& d : expiries)
            f.push_back(spot * std::exp(0.025 * dc.yearFraction(ref, d)));
        return f;
    }
    std::shared_ptr<SsviBlackVol> surface() const {
        return makeSsviSurface(
            ref, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, expiries, params, forwards(),
            spot);
    }
    Handle<QuantLib::YieldTermStructure> flat(double r) const {
        return Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(ref, r, dc));
    }
};

} // namespace

TEST(SsviPayloadSurface, ReferenceForwardRunsThroughTheSliceForwards) {
    PayloadMarket m;
    QuantLib::Settings::instance().evaluationDate() = m.ref;
    const auto s = m.surface();
    const auto f = m.forwards();
    for (std::size_t i = 0; i < m.expiries.size(); ++i) {
        const double T = m.dc.yearFraction(m.ref, m.expiries[i]);
        EXPECT_NEAR(s->forward(T), f[i], 1e-12 * f[i]);
        // At the slice expiry and forward the Black variance is the slice's theta.
        EXPECT_NEAR(s->blackVariance(T, f[i], true), m.params[i].theta, 1e-15);
    }
    EXPECT_DOUBLE_EQ(s->forward(0.0), m.spot);
    // ln F linear in time between two slices.
    const double t0 = m.dc.yearFraction(m.ref, m.expiries[1]);
    const double t1 = m.dc.yearFraction(m.ref, m.expiries[2]);
    const double tm = 0.5 * (t0 + t1);
    EXPECT_NEAR(std::log(s->forward(tm)), 0.5 * (std::log(f[1]) + std::log(f[2])), 1e-12);
    EXPECT_DOUBLE_EQ(s->lastSliceTime(), m.dc.yearFraction(m.ref, m.expiries.back()));
    EXPECT_EQ(s->expiries(), m.expiries);
}

// The note pricer's local vol on an eSSVI surface is the closed form, not QuantLib's
// finite-difference LocalVolSurface; on a process market with another forward (a dividend
// yield different from the surface's carry) it applies the sticky-strike correction.
TEST(SsviPayloadSurface, LocalVolCacheReadsTheClosedFormLocalVol) {
    PayloadMarket m;
    QuantLib::Settings::instance().evaluationDate() = m.ref;
    const auto s = m.surface();
    Handle<QuantLib::Quote> spot(std::make_shared<QuantLib::SimpleQuote>(m.spot));
    const auto rf = m.flat(0.04), dq = m.flat(0.01);
    mc::BasketAsset asset =
        mc::MakeBasketAsset("A", spot, dq, rf, Handle<QuantLib::BlackVolTermStructure>(s));
    EXPECT_FALSE(asset.exactStepCapable);
    mc::LocalVolCache cache;
    cache.attach(asset);
    ASSERT_FALSE(asset.localVol.empty());
    SsviLocalVol direct(s, spot, dq, rf);
    QuantLib::LocalVolSurface fd(Handle<QuantLib::BlackVolTermStructure>(s), rf, dq, spot);
    double worst = 0.0;
    for (double t : {0.05, 0.3, 0.6, 1.2, 1.9, 3.0}) {
        for (double S : {60.0, 80.0, 100.0, 120.0, 150.0}) {
            const double v = asset.localVol->localVol(t, S, true);
            EXPECT_DOUBLE_EQ(v, direct.localVol(t, S, true)) << t << " " << S;
            worst = std::max(worst, std::fabs(v / fd.localVol(t, S, true) - 1.0));
        }
    }
    // The closed form and QuantLib's stencil agree to the stencil's accuracy.
    EXPECT_LT(worst, 1e-4);
}

TEST(SsviPayloadSurface, ClampsAndFloorApplyToTheClosedForm) {
    PayloadMarket m;
    QuantLib::Settings::instance().evaluationDate() = m.ref;
    const auto s = m.surface();
    Handle<QuantLib::Quote> spot(std::make_shared<QuantLib::SimpleQuote>(m.spot));
    const auto rf = m.flat(0.04), dq = m.flat(0.015);
    SsviLocalVol direct(s, spot, dq, rf);

    // Strike range: a level beyond it reads the edge (margin 1e-3 in log-strike).
    mc::BasketAsset a =
        mc::MakeBasketAsset("A", spot, dq, rf, Handle<QuantLib::BlackVolTermStructure>(s));
    a.localVolStrikeRange = std::make_pair(80.0, 125.0);
    a.localVolTime = mc::LocalVolTimeExtrapolation::FlatLocalVol;
    mc::LocalVolCache cache;
    cache.attach(a);
    const double tLast = s->lastSliceTime();
    EXPECT_DOUBLE_EQ(
        a.localVol->localVol(0.5, 50.0, true), direct.localVol(0.5, 80.0 * std::exp(1e-3), true));
    EXPECT_DOUBLE_EQ(
        a.localVol->localVol(0.5, 200.0, true),
        direct.localVol(0.5, 125.0 * std::exp(-1e-3), true));
    // FlatLocalVol: held at the last slice's time - 1e-3 (the surface itself has no last date).
    EXPECT_DOUBLE_EQ(
        a.localVol->localVol(tLast + 2.0, 100.0, true), direct.localVol(tLast - 1e-3, 100.0, true));

    // Floor: max(closed form, floor), counted.
    mc::BasketAsset b =
        mc::MakeBasketAsset("B", spot, dq, rf, Handle<QuantLib::BlackVolTermStructure>(s));
    b.localVolFloor = 0.25;
    mc::LocalVolCache cacheB;
    cacheB.attach(b);
    const auto* clamped =
        dynamic_cast<const mc::EdgeClampedLocalVol*>(b.localVol.currentLink().get());
    ASSERT_NE(clamped, nullptr);
    clamped->resetCounts();
    const double low = direct.localVol(1.0, 140.0, true);
    const double high = direct.localVol(0.1, 60.0, true);
    ASSERT_LT(low, 0.25);
    ASSERT_GT(high, 0.25);
    EXPECT_DOUBLE_EQ(b.localVol->localVol(1.0, 140.0, true), 0.25);
    EXPECT_DOUBLE_EQ(b.localVol->localVol(0.1, 60.0, true), high);
    EXPECT_EQ(clamped->reads(), 2u);
    EXPECT_EQ(clamped->floored(), 1u);
    EXPECT_EQ(clamped->refusals(), 0u);
    EXPECT_DOUBLE_EQ(clamped->dupire(0.1, 60.0), high);

    // A slice that admits arbitrage: the closed form refuses (QuantLib::Error), the floor
    // takes it and counts a refusal.
    auto bad = std::make_shared<SsviBlackVol>(
        m.ref, QuantLib::NullCalendar(), QuantLib::Unadjusted, m.dc, std::vector<Date>{m.ref + 91},
        std::vector<SsviSliceParams>{{0.01, -0.7, 1.0}}, m.spot, dq, rf);
    mc::BasketAsset c =
        mc::MakeBasketAsset("C", spot, dq, rf, Handle<QuantLib::BlackVolTermStructure>(bad));
    mc::LocalVolCache cacheC;
    cacheC.attach(c);
    bool refused = false;
    for (double S : {30.0, 50.0, 70.0, 100.0, 150.0, 200.0}) {
        try {
            c.localVol->localVol(0.2, S, true);
        } catch (const QuantLib::Error&) {
            refused = true;
        }
    }
    EXPECT_TRUE(refused);
    c.localVolFloor = 0.1;
    c.localVol = Handle<QuantLib::LocalVolTermStructure>();
    mc::LocalVolCache cacheD;
    cacheD.attach(c);
    for (double S : {30.0, 50.0, 70.0, 100.0, 150.0, 200.0})
        EXPECT_GE(c.localVol->localVol(0.2, S, true), 0.1);
    const auto* cc = dynamic_cast<const mc::EdgeClampedLocalVol*>(c.localVol.currentLink().get());
    ASSERT_NE(cc, nullptr);
    EXPECT_GT(cc->refusals(), 0u);
}

// The vega bump of a note: every slice's ATM vol + shift, still an SsviBlackVol (closed-form
// local vol), forwards kept.
TEST(SsviPayloadSurface, VegaBumpStaysEssvi) {
    PayloadMarket m;
    QuantLib::Settings::instance().evaluationDate() = m.ref;
    BlackVolEntry e;
    e.handle = Handle<QuantLib::BlackVolTermStructure>(m.surface());
    e.shapeFb = quantra::enums::VolSurfaceShape_SsviSlices;
    for (double shift : {0.01, -0.01}) {
        const auto h = shiftedBlackVol(e, shift);
        const auto bumped = std::dynamic_pointer_cast<SsviBlackVol>(h.currentLink());
        ASSERT_NE(bumped, nullptr);
        const auto f = m.forwards();
        for (std::size_t i = 0; i < m.expiries.size(); ++i) {
            const double T = m.dc.yearFraction(m.ref, m.expiries[i]);
            const double atm = std::sqrt(m.params[i].theta / T) + shift;
            EXPECT_NEAR(bumped->blackVol(T, f[i], true), atm, 1e-13);
            EXPECT_DOUBLE_EQ(bumped->surface().slices()[i].psi, m.params[i].psi);
        }
    }
    EXPECT_THROW(shiftedBlackVol(e, -0.5), QuantraError);
}

// ---------------------------------------------------------------------------
// /calibrate-vol-surface, method Ssvi.
// ---------------------------------------------------------------------------

namespace {

constexpr double kSpot = 24000.0;
constexpr double kRate = 0.04;
constexpr double kDiv = 0.01;

struct SheetMarket {
    Date asOf{30, QuantLib::September, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    PricingRegistry reg;
    PricingContext ctx;
    std::shared_ptr<QuantLib::YieldTermStructure> discount;
    std::vector<Date> expiries{
        Date(16, QuantLib::October, 2026),   Date(20, QuantLib::November, 2026),
        Date(18, QuantLib::December, 2026),  Date(15, QuantLib::January, 2027),
        Date(19, QuantLib::March, 2027),     Date(18, QuantLib::June, 2027),
        Date(17, QuantLib::September, 2027), Date(17, QuantLib::December, 2027)};
    std::vector<double> atmVols{0.230, 0.225, 0.220, 0.215, 0.210, 0.210, 0.210, 0.210};

    SheetMarket() {
        QuantLib::Settings::instance().evaluationDate() = asOf;
        ctx.asOf = asOf;
        discount = std::make_shared<QuantLib::FlatForward>(asOf, kRate, dc);
        reg.rates.curves["USD_OIS"] =
            std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>(discount);
        auto placeholder = std::make_shared<QuantLib::FlatForward>(asOf, 0.0, dc);
        EquityUnderlyingRuntime u;
        u.currency = "USD";
        u.spot = Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(kSpot));
        u.dividend = Handle<QuantLib::YieldTermStructure>(placeholder);
        reg.equity.equityUnderlyings["NDX"] = u;
    }

    double t(const Date& d) const { return dc.yearFraction(asOf, d); }
    double forward(const Date& d) const { return kSpot * std::exp((kRate - kDiv) * t(d)); }
    // SSVI power law, rho = -0.7, phi = 1 / sqrt(theta): psi = sqrt(theta).
    SsviSliceParams truth(std::size_t i) const {
        const double theta = atmVols[i] * atmVols[i] * t(expiries[i]);
        return SsviSliceParams{theta, -0.7, std::sqrt(theta)};
    }
    double vol(std::size_t i, double K) const {
        const SsviSliceParams p = truth(i);
        const ssvi::Node n{p.theta, p.psi, p.rho * p.psi, 0.0, 0.0, 0.0};
        return std::sqrt(ssvi::w(n, std::log(K / forward(expiries[i]))) / t(expiries[i]));
    }
    std::vector<OptionQuoteLine> lines(double lo, double hi, double step) const {
        std::vector<OptionQuoteLine> out;
        for (std::size_t i = 0; i < expiries.size(); ++i) {
            const Date& e = expiries[i];
            const double T = t(e), F = forward(e), DF = discount->discount(e);
            for (double m = lo; m <= hi + 1e-12; m += step) {
                const double K = std::round(kSpot * m);
                const double sd = vol(i, K) * std::sqrt(T);
                const double c = QuantLib::blackFormula(QuantLib::Option::Call, K, F, sd, DF);
                const double p = QuantLib::blackFormula(QuantLib::Option::Put, K, F, sd, DF);
                OptionQuoteLine l;
                l.expiry = e;
                l.expiryIso = DateToIso(e);
                l.strike = K;
                l.callBid = c * 0.995;
                l.callAsk = c * 1.005;
                l.putBid = p * 0.995;
                l.putAsk = p * 1.005;
                out.push_back(l);
            }
        }
        return out;
    }
};

VolSurfaceCalibrationInputs ssviInputs(const SheetMarket& mkt) {
    VolSurfaceCalibrationInputs in;
    in.base.sheetId = "NDX_TEST";
    in.base.underlyingId = "NDX";
    in.base.discountingCurveId = "USD_OIS";
    in.base.dividendCurveId = "unused";
    in.method = VolSurfaceFitMethod::Ssvi;
    in.volSurfaceId = "NDX_VOL";
    in.base.quotes = mkt.lines(0.70, 1.30, 0.025);
    return in;
}

} // namespace

TEST(VolSurfaceCalibrationSsvi, RecoversAnSsviSurfaceArbitrageFree) {
    SheetMarket mkt;
    const auto in = ssviInputs(mkt);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    ASSERT_EQ(r.method, VolSurfaceFitMethod::Ssvi);
    ASSERT_EQ(r.expiries.size(), 8u);
    unsigned used = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto& e = r.expiries[i];
        ASSERT_TRUE(e.usable) << e.droppedReason;
        ASSERT_TRUE(e.ssvi.has_value());
        const SsviSliceParams truth = mkt.truth(i);
        EXPECT_NEAR(e.ssvi->theta, truth.theta, 1e-9 + 1e-7 * truth.theta) << e.expiryIso;
        EXPECT_NEAR(e.ssvi->rho, truth.rho, 1e-6) << e.expiryIso;
        EXPECT_NEAR(e.ssvi->psi, truth.psi, 1e-6) << e.expiryIso;
        EXPECT_LT(*e.rmseVol, 1e-7);
        EXPECT_FALSE(e.ssviAnchorClamped);
        ASSERT_TRUE(e.ssviMarginButterflyPsi && e.ssviMarginButterflyPsi2);
        EXPECT_GE(*e.ssviMarginButterflyPsi, 0.0);
        EXPECT_GE(*e.ssviMarginButterflyPsi2, 0.0);
        EXPECT_EQ(e.ssviMarginCalendarTheta.has_value(), i > 0);
        if (i > 0) {
            EXPECT_GE(*e.ssviMarginCalendarTheta, 0.0);
            EXPECT_GE(*e.ssviMarginCalendarPsi, 0.0);
            EXPECT_GE(*e.ssviMarginCalendarA, 0.0);
        }
        // No strike range on an eSSVI slice: the payload is read unclamped.
        EXPECT_FALSE(e.strikeMin.has_value() || e.strikeMax.has_value());
        EXPECT_FALSE(e.svi.has_value());
        used += e.quotesUsed;
        // The grid is the surface sampled at the slice expiries.
        for (std::size_t j = 0; j < r.gridStrikes.size(); ++j) {
            EXPECT_NEAR(
                r.gridVols[i * r.gridStrikes.size() + j], mkt.vol(i, r.gridStrikes[j]), 1e-6)
                << e.expiryIso << " " << r.gridStrikes[j];
        }
    }
    ASSERT_TRUE(r.ssviArbitrage.has_value());
    EXPECT_TRUE(r.ssviArbitrage->violations.empty());
    EXPECT_GE(r.ssviArbitrage->scanMinG, 0.0);
    EXPECT_GE(r.ssviArbitrage->scanMinDwDt, 0.0);
    ASSERT_TRUE(r.localVolCheckSsvi.has_value());
    EXPECT_TRUE(r.localVolCheckSsvi->arbitrageFree) << r.localVolCheckSsvi->firstRefusal;
    EXPECT_EQ(r.localVolCheckSsvi->refused, 0u);
    EXPECT_EQ(r.localVolCheckSsvi->points, r.localVolCheckSsvi->times * 201u);
    EXPECT_FALSE(r.localVolCheckSvi.has_value());
    EXPECT_EQ(r.fit.count, used);
    EXPECT_EQ(r.fit.inside, used);
}

TEST(VolSurfaceCalibrationSsvi, GridBetweenExpiriesAndMoneynessCut) {
    SheetMarket mkt;
    auto in = ssviInputs(mkt);
    // Any grid date up to the last expiry: the parameters are interpolated in time.
    in.gridExpiries = {QuantLib::Period(30, QuantLib::Days), QuantLib::Period(200, QuantLib::Days)};
    in.ssviMaxAbsLogMoneyness = 0.15;
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    ASSERT_EQ(r.gridExpiryIso.size(), 2u);
    unsigned cut = 0;
    for (const auto& e : r.expiries) {
        for (const auto& d : e.dropped)
            if (d.reason == "max_abs_log_moneyness") ++cut;
    }
    EXPECT_GT(cut, 0u);
    // The surface still reproduces the truth at the quotes it kept.
    for (const auto& e : r.expiries)
        EXPECT_LT(*e.rmseVol, 1e-7) << e.expiryIso;
    // Fewer quotes left than the slice needs: a 400 naming the expiry.
    in.ssviMaxAbsLogMoneyness = 1e-4;
    EXPECT_THROW(VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx), QuantraError);
}

}} // namespace quantra::testing
