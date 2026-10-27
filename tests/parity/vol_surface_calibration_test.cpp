// Black vol surface from option quotes: evaluator tests.
//
// Quotes are generated here from known surfaces with QuantLib's blackFormula
// (exact mids, 1 % bid/ask) so the fit has a truth to be compared with:
//   * an SSVI power-law surface written per expiry as raw SVI parameters
//     (rho = -0.7, the equity skew) -> SviPerExpiry recovers it, Andreasen-
//     Huge reproduces it inside the quoted strike range;
//   * a flat 20 % -> both methods return 20 % everywhere;
//   * a crafted calendar violation (total variance falling) -> reported and
//     clipped, or reported only;
//   * a crafted butterfly violation (an SVI slice with Durrleman g < 0) ->
//     reported and refitted with the penalty;
//   * forwards pasted from the implied-forward fit give the same surface as
//     the internal fit; the 400s and 404s.
#include "date_convert.h"
#include "error.h"
#include "implied_forwards_evaluator.h"
#include "vol_surface_calibration_evaluator.h"

#include <ql/pricingengines/blackformula.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;

constexpr double kSpot = 24000.0;
constexpr double kRate = 0.04;
constexpr double kDiv = 0.01;

// SSVI power-law surface (eta = 1, gamma = 1/2) as raw SVI per expiry.
SviParams ssviSlice(double atmVol, double T, double rho) {
    const double theta = atmVol * atmVol * T;
    const double phi = 1.0 / std::sqrt(theta);
    SviParams p;
    p.a = theta * (1.0 - rho * rho) / 2.0;
    p.b = theta * phi / 2.0;
    p.sigma = std::sqrt(1.0 - rho * rho) / phi;
    p.rho = rho;
    p.m = -rho / phi;
    return p;
}

struct Market {
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

    Market() {
        QuantLib::Settings::instance().evaluationDate() = asOf;
        ctx.asOf = asOf;
        discount = std::make_shared<QuantLib::FlatForward>(asOf, kRate, dc);
        reg.rates.curves["USD_OIS"] =
            std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>(discount);
        auto placeholder = std::make_shared<QuantLib::FlatForward>(asOf, 0.0, dc);
        reg.rates.curves["PLACEHOLDER"] =
            std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>(placeholder);
        EquityUnderlyingRuntime u;
        u.currency = "USD";
        u.spot = Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(kSpot));
        u.dividend = Handle<QuantLib::YieldTermStructure>(placeholder);
        reg.equity.equityUnderlyings["NDX"] = u;
    }

    double t(const Date& d) const { return dc.yearFraction(asOf, d); }
    double df(const Date& d) const { return discount->discount(d); }
    double forward(const Date& d) const { return kSpot * std::exp((kRate - kDiv) * t(d)); }

    // Implied vol of the truth at (expiry, strike): vol(k, T).
    using VolFn = std::function<double(double k, double T, std::size_t expiryIdx)>;

    VolFn ssvi(double rho = -0.7) const {
        return [this, rho](double k, double T, std::size_t i) {
            return std::sqrt(sviTotalVariance(ssviSlice(atmVols[i], T, rho), k) / T);
        };
    }
    static VolFn flat(double v) {
        return [v](double, double, std::size_t) { return v; };
    }

    std::vector<OptionQuoteLine> lines(
        const VolFn& vol,
        const std::vector<double>& moneyness,
        double spread = 0.01,
        std::size_t nExpiries = 8) const {
        std::vector<OptionQuoteLine> out;
        for (std::size_t i = 0; i < nExpiries; ++i) {
            const Date& expiry = expiries[i];
            const double T = t(expiry), F = forward(expiry), DF = df(expiry);
            for (double m : moneyness) {
                const double K = std::round(kSpot * m);
                const double k = std::log(K / F);
                const double sd = vol(k, T, i) * std::sqrt(T);
                const double c = QuantLib::blackFormula(QuantLib::Option::Call, K, F, sd, DF);
                const double p = QuantLib::blackFormula(QuantLib::Option::Put, K, F, sd, DF);
                OptionQuoteLine l;
                l.expiry = expiry;
                l.expiryIso = DateToIso(expiry);
                l.strike = K;
                l.callBid = c * (1.0 - spread / 2);
                l.callAsk = c * (1.0 + spread / 2);
                l.putBid = p * (1.0 - spread / 2);
                l.putAsk = p * (1.0 + spread / 2);
                out.push_back(l);
            }
        }
        return out;
    }
};

std::vector<double> grid(double lo, double hi, double step) {
    std::vector<double> ms;
    for (double m = lo; m <= hi + 1e-12; m += step)
        ms.push_back(std::round(m * 1e6) / 1e6);
    return ms;
}

VolSurfaceCalibrationInputs baseInputs(VolSurfaceFitMethod method) {
    VolSurfaceCalibrationInputs in;
    in.base.sheetId = "NDX_TEST";
    in.base.underlyingId = "NDX";
    in.base.discountingCurveId = "USD_OIS";
    in.base.dividendCurveId = "unused";
    in.method = method;
    in.volSurfaceId = "NDX_VOL";
    return in;
}

const std::vector<double> kQuotedMoneyness = grid(0.70, 1.30, 0.025);

double gridVol(const VolSurfaceCalibrationResult& r, std::size_t i, std::size_t j) {
    return r.gridVols[i * r.gridStrikes.size() + j];
}

// Quotes of one expiry whose out-of-the-money mid is at least 1e-5 of the
// spot (the quote-resolution rule): the number the fit uses.
unsigned informative(const std::vector<OptionQuoteLine>& lines, const Market& mkt, const Date& d) {
    unsigned n = 0;
    const double F = mkt.forward(d);
    for (const auto& l : lines) {
        if (l.expiry != d) continue;
        const double mid =
            *l.strike >= F ? 0.5 * (*l.callBid + *l.callAsk) : 0.5 * (*l.putBid + *l.putAsk);
        if (mid >= 1e-5 * kSpot) ++n;
    }
    return n;
}

} // namespace

// (a) SVI truth: SviPerExpiry recovers the slices; Andreasen-Huge within
// 0.25 vol points inside the quoted strike range on informative quotes.
TEST(VolSurfaceCalibration, SviPerExpiryRecoversAnSsviSurface) {
    Market mkt;
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(mkt.ssvi(), kQuotedMoneyness);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);

    ASSERT_EQ(r.method, VolSurfaceFitMethod::SviPerExpiry);
    ASSERT_EQ(r.forwardsSource, "fitted");
    ASSERT_EQ(r.gridExpiryIso.size(), 8u);
    ASSERT_EQ(r.gridStrikes.size(), 41u); // default 0.50 .. 1.50 step 0.025
    EXPECT_NEAR(r.moneyness.front(), 0.50, 1e-12);
    EXPECT_NEAR(r.moneyness.back(), 1.50, 1e-12);
    EXPECT_NEAR(r.gridStrikes[20], kSpot, 1e-9);
    double maxErr = 0.0;
    unsigned used = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto& e = r.expiries[i];
        ASSERT_TRUE(e.usable) << e.droppedReason;
        ASSERT_TRUE(e.svi.has_value());
        const SviParams truth = ssviSlice(mkt.atmVols[i], mkt.t(mkt.expiries[i]), -0.7);
        EXPECT_NEAR(e.svi->rho, truth.rho, 1e-6) << e.expiryIso;
        EXPECT_NEAR(e.svi->b, truth.b, 1e-6) << e.expiryIso;
        EXPECT_NEAR(e.svi->m, truth.m, 1e-5) << e.expiryIso;
        EXPECT_FALSE(e.butterflyViolation);
        EXPECT_EQ(e.butterflyAction, "none");
        EXPECT_EQ(e.calendarViolations, 0u);
        EXPECT_LT(*e.rmseVol, 1e-8);
        EXPECT_EQ(e.quotesUsed, informative(in.base.quotes, mkt, mkt.expiries[i]));
        EXPECT_GE(e.quotesUsed, 15u);
        used += e.quotesUsed;
        // Whole grid, wings included: SVI extrapolates the truth's own form.
        for (std::size_t j = 0; j < r.gridStrikes.size(); ++j) {
            const double k = std::log(r.gridStrikes[j] / mkt.forward(mkt.expiries[i]));
            const double tv = mkt.ssvi()(k, mkt.t(mkt.expiries[i]), i);
            maxErr = std::max(maxErr, std::fabs(gridVol(r, i, j) - tv));
        }
    }
    EXPECT_LT(maxErr, 0.001) << "max grid error in vol units: " << maxErr; // 0.1 vol point
    // Every used quote repriced on the returned surface lies inside its bid/ask.
    EXPECT_EQ(r.repricing.count, used);
    EXPECT_GT(used, 150u);
    EXPECT_EQ(r.repricing.insideBidAsk, used);
    EXPECT_LT(r.repricing.maxAbsVolError, 1e-6);
}

TEST(VolSurfaceCalibration, AndreasenHugeReproducesAnSsviSurfaceInsideTheQuotedRange) {
    Market mkt;
    auto in = baseInputs(VolSurfaceFitMethod::AndreasenHuge);
    in.base.quotes = mkt.lines(mkt.ssvi(), kQuotedMoneyness);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);

    ASSERT_TRUE(r.andreasenHugeError.has_value());
    EXPECT_LT(r.andreasenHugeError->avgVol, 0.001);
    EXPECT_LT(r.andreasenHugeError->maxVol, 0.01);
    double maxErr = 0.0, maxErrInformative = 0.0;
    unsigned compared = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        const auto& e = r.expiries[i];
        ASSERT_TRUE(e.usable) << e.droppedReason;
        EXPECT_FALSE(e.svi.has_value());
        EXPECT_EQ(e.quotesUsed, informative(in.base.quotes, mkt, mkt.expiries[i]));
        const double T = mkt.t(mkt.expiries[i]), F = mkt.forward(mkt.expiries[i]);
        for (std::size_t j = 0; j < r.gridStrikes.size(); ++j) {
            const double K = r.gridStrikes[j];
            if (K < 0.70 * kSpot - 1e-9 || K > 1.30 * kSpot + 1e-9) continue; // quoted range only
            const double k = std::log(K / F);
            const double tv = mkt.ssvi()(k, T, i);
            const double err = std::fabs(gridVol(r, i, j) - tv);
            maxErr = std::max(maxErr, err);
            // Informative quote: the out-of-the-money Black price at the truth
            // vol is at least 1e-4 of the spot (2.4 index points); below that
            // the implied vol is not determined by a 1 % bid/ask.
            const double otm = QuantLib::blackFormula(
                K >= F ? QuantLib::Option::Call : QuantLib::Option::Put, K, F, tv * std::sqrt(T),
                mkt.df(mkt.expiries[i]));
            if (otm >= 1e-4 * kSpot) {
                maxErrInformative = std::max(maxErrInformative, err);
                ++compared;
            }
        }
    }
    EXPECT_GT(compared, 60u);
    EXPECT_LT(maxErrInformative, 0.0025)
        << "max grid error on informative quotes, vol units: " << maxErrInformative;
    // Report what the whole quoted range gives (the deep short-dated wings
    // hold prices of 1e-6 of the spot where any vol prices inside the spread).
    RecordProperty("max_error_quoted_range_vol", maxErr);
    RecordProperty("max_error_informative_vol", maxErrInformative);
    // Repricing on the returned grid: at least 95 % of the used quotes inside
    // their 1 % bid/ask (180 of 189 observed; the rest are short-dated wing
    // quotes priced below 1e-4 of the spot, where the model's price noise is
    // a visible fraction of the spread).
    EXPECT_GT(r.repricing.count, 150u);
    EXPECT_GE(r.repricing.insideBidAsk * 100u, r.repricing.count * 95u)
        << "inside bid/ask: " << r.repricing.insideBidAsk << " of " << r.repricing.count;
    RecordProperty("inside_bid_ask", r.repricing.insideBidAsk);
    RecordProperty("repriced", r.repricing.count);
}

// (b) flat 20 %: SVI returns 20 % on the whole default grid within 0.1 vol
// point; Andreasen-Huge within 0.1 vol point on the quoted strike range
// (70 % to 130 %) and within 1 vol point beyond it, where its local-vol
// extrapolation is what the surface says (0.7 vol point observed at 55 %
// moneyness, 15 months).
TEST(VolSurfaceCalibration, FlatTwentyPercentIsRecoveredByBothMethods) {
    Market mkt;
    for (auto method : {VolSurfaceFitMethod::AndreasenHuge, VolSurfaceFitMethod::SviPerExpiry}) {
        auto in = baseInputs(method);
        in.base.quotes = mkt.lines(Market::flat(0.20), kQuotedMoneyness);
        const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
        double maxErr = 0.0, maxErrQuoted = 0.0;
        for (std::size_t i = 0; i < r.gridExpiryIso.size(); ++i) {
            for (std::size_t j = 0; j < r.gridStrikes.size(); ++j) {
                const double err = std::fabs(gridVol(r, i, j) - 0.20);
                maxErr = std::max(maxErr, err);
                if (r.gridStrikes[j] >= 0.70 * kSpot - 1e-9 &&
                    r.gridStrikes[j] <= 1.30 * kSpot + 1e-9)
                    maxErrQuoted = std::max(maxErrQuoted, err);
            }
        }
        const bool svi = method == VolSurfaceFitMethod::SviPerExpiry;
        EXPECT_LT(maxErrQuoted, 1e-3) << "method " << static_cast<int>(method)
                                      << " quoted-range max |vol - 0.20| " << maxErrQuoted;
        EXPECT_LT(maxErr, svi ? 1e-3 : 1e-2)
            << "method " << static_cast<int>(method) << " max |vol - 0.20| " << maxErr;
        RecordProperty(svi ? "svi_max_error" : "ah_max_error", maxErr);
        for (const auto& e : r.expiries) {
            EXPECT_TRUE(e.usable);
            EXPECT_EQ(e.calendarViolations, 0u) << e.expiryIso;
            EXPECT_FALSE(e.butterflyViolation);
        }
        EXPECT_EQ(r.repricing.insideBidAsk, r.repricing.count);
    }
}

// (c) calendar arbitrage: a 30 % slice followed by a 10 % one has total
// variance falling (0.09 * 0.044 > 0.01 * 0.14); SviPerExpiry reports it at
// every grid strike and clips the second slice to the first's total
// variance + 1e-8 (or reports only with enforce_calendar false).
TEST(VolSurfaceCalibration, CalendarViolationIsReportedAndClipped) {
    Market mkt;
    auto twoLevels = [](double k, double, std::size_t i) { return i == 0 ? 0.30 : 0.10; };
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(twoLevels, kQuotedMoneyness, 0.01, 2);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    ASSERT_EQ(r.expiries.size(), 2u);
    EXPECT_EQ(r.expiries[0].calendarViolations, 0u);
    EXPECT_EQ(r.expiries[1].calendarViolations, 41u);
    EXPECT_NE(r.expiries[1].calendarAction.find("clipped"), std::string::npos);
    const double T0 = mkt.t(mkt.expiries[0]), T1 = mkt.t(mkt.expiries[1]);
    const double w0 = 0.30 * 0.30 * T0;
    for (std::size_t j = 0; j < r.gridStrikes.size(); ++j) {
        EXPECT_NEAR(gridVol(r, 0, j), 0.30, 1e-6);
        EXPECT_NEAR(gridVol(r, 1, j), std::sqrt((w0 + 1e-8) / T1), 1e-6);
        EXPECT_GE(
            gridVol(r, 1, j) * gridVol(r, 1, j) * T1, gridVol(r, 0, j) * gridVol(r, 0, j) * T0);
    }
    EXPECT_NEAR(*r.expiries[1].calendarMaxClipVol, std::sqrt((w0 + 1e-8) / T1) - 0.10, 1e-6);

    in.enforceCalendar = false;
    const auto r2 = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    EXPECT_EQ(r2.expiries[1].calendarViolations, 41u);
    EXPECT_NE(r2.expiries[1].calendarAction.find("reported only"), std::string::npos);
    EXPECT_NEAR(gridVol(r2, 1, 20), 0.10, 1e-6);

    // Andreasen-Huge is arbitrage-free by construction: the second slice
    // cannot be fitted to a lower total variance, so the quotes there
    // reprice outside their bid/ask and the report says so (no clip).
    auto inAh = baseInputs(VolSurfaceFitMethod::AndreasenHuge);
    inAh.base.quotes = in.base.quotes;
    const auto r3 = VolSurfaceCalibrationEvaluator().evaluate(inAh, mkt.reg, mkt.ctx);
    EXPECT_LT(r3.repricing.insideBidAsk, r3.repricing.count);
    // Total variance never falls by more than the implied-vol inversion
    // tolerance (1e-5 in variance = 0.1 vol point at 16 days), and the
    // report says "reported only" where it does.
    for (std::size_t j = 0; j < r3.gridStrikes.size(); ++j) {
        EXPECT_GE(
            gridVol(r3, 1, j) * gridVol(r3, 1, j) * T1,
            gridVol(r3, 0, j) * gridVol(r3, 0, j) * T0 - 1e-5);
    }
    if (r3.expiries[1].calendarViolations > 0) {
        EXPECT_NE(r3.expiries[1].calendarAction.find("reported only"), std::string::npos);
    }
}

// Butterfly arbitrage: an SVI slice whose Durrleman g is -0.41 near the money
// is reported and refitted with the penalty to g >= 0.
TEST(VolSurfaceCalibration, ButterflyViolationIsReportedAndRefitted) {
    Market mkt;
    SviParams bad;
    bad.a = 0.02;
    bad.b = 0.6;
    bad.sigma = 0.05;
    bad.rho = -0.6;
    bad.m = 0.1;
    // One-year slice: put it at the 2027-09-17 expiry (T ~ 0.96) alone.
    Market one = mkt;
    one.expiries = {Date(17, QuantLib::September, 2027)};
    one.atmVols = {0.2};
    const double T = one.t(one.expiries[0]);
    auto badVol = [&](double k, double, std::size_t) {
        return std::sqrt(sviTotalVariance(bad, k) / T);
    };
    ASSERT_LT(sviDurrlemanG(bad, -0.07), 0.0);

    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = one.lines(badVol, kQuotedMoneyness, 0.01, 1);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, one.reg, one.ctx);
    ASSERT_EQ(r.expiries.size(), 1u);
    const auto& e = r.expiries[0];
    ASSERT_TRUE(e.usable) << e.droppedReason;
    EXPECT_TRUE(e.butterflyViolation);
    EXPECT_EQ(e.butterflyAction.rfind("refit with penalty", 0), 0u) << e.butterflyAction;
    EXPECT_GE(*e.butterflyMinG, -1e-6) << e.butterflyAction;
    // The refitted slice is a different, arbitrage-free smile: it no longer
    // reprices every quote, and says so.
    EXPECT_LT(r.repricing.insideBidAsk, r.repricing.count);

    in.enforceButterfly = false;
    const auto r2 = VolSurfaceCalibrationEvaluator().evaluate(in, one.reg, one.ctx);
    EXPECT_TRUE(r2.expiries[0].butterflyViolation);
    EXPECT_LT(*r2.expiries[0].butterflyMinG, -0.3);
    EXPECT_NE(r2.expiries[0].butterflyAction.find("reported only"), std::string::npos);
    EXPECT_NEAR(r2.expiries[0].svi->b, bad.b, 1e-4);
    EXPECT_EQ(r2.repricing.insideBidAsk, r2.repricing.count);
}

// Forwards pasted from the implied-forward fit give the same surface as the
// internal fit, and a custom grid is honoured.
TEST(VolSurfaceCalibration, PastedForwardsAndCustomGrid) {
    Market mkt;
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(mkt.ssvi(), kQuotedMoneyness);
    const auto fitted = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);

    ImpliedForwardsInputs fin = in.base;
    const auto fwd = ImpliedForwardsEvaluator().evaluate(fin, mkt.reg, mkt.ctx);
    std::vector<VolSurfaceForwardInput> pasted;
    for (const auto& e : fwd.expiries) {
        VolSurfaceForwardInput f;
        f.expiryIso = e.expiryIso;
        f.expiry = e.expiry;
        f.forward = *e.forward;
        f.impliedDiscountFactor = *e.impliedDiscountFactor;
        pasted.push_back(f);
    }
    in.forwards = pasted;
    in.moneyness = {0.8, 0.9, 1.0, 1.1, 1.2};
    in.gridExpiries = {QuantLib::Period(16, QuantLib::Days), QuantLib::Period(170, QuantLib::Days)};
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    EXPECT_EQ(r.forwardsSource, "request");
    ASSERT_EQ(r.gridExpiryIso, (std::vector<std::string>{"2026-10-16", "2027-03-19"}));
    ASSERT_EQ(r.gridExpiryDays, (std::vector<int>{16, 170}));
    ASSERT_EQ(r.gridStrikes.size(), 5u);
    EXPECT_NEAR(r.gridStrikes[0], 0.8 * kSpot, 1e-9);
    // Same slices: the 1.0 strike of the first expiry equals the fitted run's.
    EXPECT_NEAR(gridVol(r, 0, 2), gridVol(fitted, 0, 20), 1e-12);
    EXPECT_NEAR(gridVol(r, 1, 2), gridVol(fitted, 4, 20), 1e-12);
    EXPECT_EQ(r.repricing.count, fitted.repricing.count);

    // SviPerExpiry: a grid expiry off the sheet is a 400; Andreasen-Huge
    // accepts it.
    in.gridExpiries = {QuantLib::Period(100, QuantLib::Days)};
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx), QuantraInvalidArgument);
    in.method = VolSurfaceFitMethod::AndreasenHuge;
    const auto rAh = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    ASSERT_EQ(rAh.gridExpiryIso, (std::vector<std::string>{"2027-01-08"}));
    EXPECT_GT(gridVol(rAh, 0, 2), 0.15);
    EXPECT_LT(gridVol(rAh, 0, 2), 0.30);
    // Beyond the last fitted expiry: 400.
    in.gridExpiries = {QuantLib::Period(2, QuantLib::Years)};
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx), QuantraInvalidArgument);
}

// Dropped quotes with reasons: a mid above the Black upper bound, a mid below
// the quote resolution, a thin SVI expiry, an expiry without a pasted forward.
TEST(VolSurfaceCalibration, DropsWithReasons) {
    Market mkt;
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(mkt.ssvi(), kQuotedMoneyness, 0.01, 3);
    // Two parity-consistent lines at the first expiry (C - P = DF (F - K), so
    // the forward fit is untouched): an out-of-the-money put quoted above its
    // Black upper bound DF * K (no vol solves), and one quoted at a tenth of
    // a point (below the quote resolution 1e-5 * spot).
    const double F0 = mkt.forward(mkt.expiries[0]), DF0 = mkt.df(mkt.expiries[0]);
    OptionQuoteLine z = in.base.quotes[2];
    z.strike = 16000.0;
    z.putBid = 17000.0;
    z.putAsk = 17010.0;
    z.callBid = 17005.0 + DF0 * (F0 - 16000.0) - 5.0;
    z.callAsk = z.callBid.value() + 10.0;
    in.base.quotes.push_back(z);
    OptionQuoteLine tiny = z;
    tiny.strike = 16100.0;
    tiny.putBid = 0.0999;
    tiny.putAsk = 0.1001;
    tiny.callBid = 0.10 + DF0 * (F0 - 16100.0) - 0.5;
    tiny.callAsk = tiny.callBid.value() + 1.0;
    in.base.quotes.push_back(tiny);
    // The third expiry with four near-the-money strikes: enough for the
    // forward fit (min_quotes_per_expiry 4), too thin for a 5-parameter SVI.
    in.base.quotes.erase(
        std::remove_if(
            in.base.quotes.begin(), in.base.quotes.end(),
            [&](const OptionQuoteLine& l) {
                return l.expiry == mkt.expiries[2] &&
                       (*l.strike < 0.94 * kSpot || *l.strike > 1.03 * kSpot);
            }),
        in.base.quotes.end());
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    ASSERT_EQ(r.expiries.size(), 3u);
    EXPECT_TRUE(r.expiries[0].usable);
    EXPECT_EQ(r.expiries[0].quotesUsed, informative(in.base.quotes, mkt, mkt.expiries[0]) - 1);
    bool found = false, foundTiny = false;
    for (const auto& d : r.expiries[0].dropped) {
        if (d.strike && *d.strike == 16000.0) {
            found = true;
            EXPECT_NE(d.reason.find("does not solve"), std::string::npos) << d.reason;
        }
        if (d.strike && *d.strike == 16100.0) {
            foundTiny = true;
            EXPECT_NE(d.reason.find("quote resolution"), std::string::npos) << d.reason;
        }
    }
    EXPECT_TRUE(found);
    EXPECT_TRUE(foundTiny);
    EXPECT_FALSE(r.expiries[2].usable);
    EXPECT_NE(r.expiries[2].droppedReason.find("fewer than 5"), std::string::npos)
        << r.expiries[2].droppedReason;
    ASSERT_EQ(r.gridExpiryIso.size(), 2u);

    // Pasted forwards that miss the second expiry.
    std::vector<VolSurfaceForwardInput> pasted;
    VolSurfaceForwardInput f;
    f.expiryIso = DateToIso(mkt.expiries[0]);
    f.expiry = mkt.expiries[0];
    f.forward = mkt.forward(mkt.expiries[0]);
    f.impliedDiscountFactor = mkt.df(mkt.expiries[0]);
    pasted.push_back(f);
    in.forwards = pasted;
    const auto r2 = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    EXPECT_FALSE(r2.expiries[1].usable);
    EXPECT_NE(r2.expiries[1].droppedReason.find("no forward"), std::string::npos);
    EXPECT_EQ(r2.forwards.size(), 1u);
}

TEST(VolSurfaceCalibration, RejectedRequests) {
    Market mkt;
    auto in = baseInputs(VolSurfaceFitMethod::AndreasenHuge);
    in.base.quotes = mkt.lines(mkt.ssvi(), kQuotedMoneyness, 0.01, 2);

    auto bad = in;
    bad.base.style = OptionSheetStyle::American;
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraInvalidArgument);
    bad = in;
    bad.moneyness = {1.0, 0.9};
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraInvalidArgument);
    bad = in;
    bad.moneyness = {0.0, 1.0};
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraInvalidArgument);
    bad = in;
    bad.base.filter.maxRelativeSpread = 0.0;
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraInvalidArgument);
    bad = in;
    bad.forwards = std::vector<VolSurfaceForwardInput>{};
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraInvalidArgument);
    bad = in;
    VolSurfaceForwardInput f;
    f.expiryIso = DateToIso(mkt.expiries[0]);
    f.expiry = mkt.expiries[0];
    f.forward = -1.0;
    f.impliedDiscountFactor = 0.99;
    bad.forwards = std::vector<VolSurfaceForwardInput>{f};
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraInvalidArgument);
    bad = in;
    bad.base.underlyingId = "SPX";
    EXPECT_THROW(VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraNotFound);
    bad = in;
    bad.base.discountingCurveId = "EUR";
    EXPECT_THROW(VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraNotFound);
    // Crossed quotes everywhere: no expiry survives.
    bad = in;
    for (auto& l : bad.base.quotes)
        l.callBid = *l.callAsk + 1.0;
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(bad, mkt.reg, mkt.ctx), QuantraInvalidArgument);
}

}} // namespace quantra::testing
