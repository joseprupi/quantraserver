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
#include "smile_slice_vol_surface.h"
#include "ssvi_vol_surface.h"
#include "vol_surface_calibration_evaluator.h"
#include "vol_surface_validation_evaluator.h"

#include <ql/pricingengines/blackformula.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
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
    EXPECT_EQ(r.fit.count, used);
    EXPECT_GT(used, 150u);
    EXPECT_EQ(r.fit.inside, used);
    EXPECT_LT(r.fit.maxAbsVolError, 1e-6);
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
    EXPECT_GT(r.fit.count, 150u);
    EXPECT_GE(r.fit.inside * 100u, r.fit.count * 95u)
        << "inside bid/ask: " << r.fit.inside << " of " << r.fit.count;
    RecordProperty("inside_bid_ask", r.fit.inside);
    RecordProperty("repriced", r.fit.count);
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
        // Andreasen-Huge on its own finite-difference strikes: the model's
        // price noise puts at most one wing quote just outside its 1 %
        // spread.
        EXPECT_GE(r.fit.inside + (svi ? 0u : 1u), r.fit.count)
            << "method " << static_cast<int>(method);
    }
}

// (c) calendar arbitrage: a 30 % slice followed by a 10 % one has total
// variance falling (0.09 * 0.044 > 0.01 * 0.14). SviPerExpiry fits from the
// last expiry back: the first slice is refitted under the second (minus the
// margin min_forward_vol^2 dT, QuantLib's local-vol time derivative along
// the forward non-negative), nothing is left for the grid clip, and the
// local vol of both returned surfaces evaluates everywhere on the scan.
// With enforce_calendar false the independent fits are reported (no refit,
// no clip) and the local vol check fails.
TEST(VolSurfaceCalibration, CalendarViolationIsRefittedUnderTheNextExpiry) {
    Market mkt;
    auto twoLevels = [](double k, double, std::size_t i) { return i == 0 ? 0.30 : 0.10; };
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(twoLevels, kQuotedMoneyness, 0.01, 2);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    ASSERT_EQ(r.expiries.size(), 2u);
    const auto& e0 = r.expiries[0];
    const auto& e1 = r.expiries[1];
    EXPECT_TRUE(e0.calendarRefit);
    EXPECT_NE(
        e0.calendarFitAction.find("refit with calendar penalty under 2026-11-20"),
        std::string::npos)
        << e0.calendarFitAction;
    EXPECT_GT(e0.calendarRefitMaxVolShift.value_or(-1.0), 0.1);
    EXPECT_FALSE(e1.calendarRefit);
    ASSERT_TRUE(e1.calendarMinMargin.has_value());
    EXPECT_GE(*e1.calendarMinMargin, 0.0);
    EXPECT_GE(e1.calendarMinForwardVol.value_or(-1.0), in.minForwardVol);
    EXPECT_EQ(e1.calendarViolations, 0u) << e1.calendarAction;
    // The second slice is the market's 10 %; the first sits under it.
    const double T0 = mkt.t(mkt.expiries[0]), T1 = mkt.t(mkt.expiries[1]);
    for (std::size_t j = 0; j < r.gridStrikes.size(); ++j) {
        if (r.gridStrikes[j] < 0.70 * kSpot || r.gridStrikes[j] > 1.30 * kSpot) continue;
        EXPECT_NEAR(gridVol(r, 1, j), 0.10, 1e-4);
        EXPECT_LT(gridVol(r, 0, j) * gridVol(r, 0, j) * T0, 0.01 * T1);
    }
    EXPECT_TRUE(r.localVolCheck.arbitrageFree) << r.localVolCheck.firstRefusal;
    ASSERT_TRUE(r.localVolCheckSvi.has_value());
    EXPECT_TRUE(r.localVolCheckSvi->arbitrageFree) << r.localVolCheckSvi->firstRefusal;
    EXPECT_EQ(r.localVolCheckSvi->refused, 0u);
    // The returned slices carry the checked region as their strike range.
    EXPECT_LT(e0.strikeMin.value_or(1.0), e0.strikeMax.value_or(0.0));
    ASSERT_EQ(r.gridLocalVolStrikeMin.size(), 2u);

    in.enforceCalendar = false;
    const auto r2 = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    EXPECT_FALSE(r2.expiries[0].calendarRefit);
    EXPECT_NE(r2.expiries[0].calendarFitAction.find("reported only"), std::string::npos);
    EXPECT_GT(r2.expiries[1].calendarViolations, 0u);
    EXPECT_NE(r2.expiries[1].calendarAction.find("reported only"), std::string::npos);
    EXPECT_LT(r2.expiries[1].calendarMinMargin.value_or(1.0), 0.0);
    EXPECT_NEAR(gridVol(r2, 0, 20), 0.30, 1e-6);
    EXPECT_NEAR(gridVol(r2, 1, 20), 0.10, 1e-6);
    ASSERT_TRUE(r2.localVolCheckSvi.has_value());
    EXPECT_FALSE(r2.localVolCheckSvi->arbitrageFree);
    EXPECT_GT(r2.localVolCheckSvi->refused, 0u);
    EXPECT_NE(r2.localVolCheckSvi->firstRefusal.find("decreasing variance"), std::string::npos)
        << r2.localVolCheckSvi->firstRefusal;

    // Andreasen-Huge is arbitrage-free by construction: the second slice
    // cannot be fitted to a lower total variance, so the quotes there
    // reprice outside their bid/ask (no refit, no clip).
    auto inAh = baseInputs(VolSurfaceFitMethod::AndreasenHuge);
    inAh.base.quotes = in.base.quotes;
    const auto r3 = VolSurfaceCalibrationEvaluator().evaluate(inAh, mkt.reg, mkt.ctx);
    EXPECT_LT(r3.fit.inside, r3.fit.count);
    for (std::size_t j = 0; j < r3.gridStrikes.size(); ++j) {
        EXPECT_GE(
            gridVol(r3, 1, j) * gridVol(r3, 1, j) * T1,
            gridVol(r3, 0, j) * gridVol(r3, 0, j) * T0 - 1e-5);
    }
    if (r3.expiries[1].calendarViolations > 0) {
        EXPECT_NE(r3.expiries[1].calendarAction.find("reported only"), std::string::npos);
    }
}

// Daily expiries fitted one by one cross: an SSVI sheet with every third
// expiry quoted 6 % low (an inverted pair in total variance at the money)
// comes back with every pair ordered by at least the margin, both returned
// surfaces' local vol evaluable on the whole scan, and the fit still within
// the spread where the quotes are ordered.
TEST(VolSurfaceCalibration, SequentialFitOrdersDisorderedExpiriesWithTheMargin) {
    Market mkt;
    mkt.expiries.clear();
    mkt.atmVols.clear();
    for (int d = 7; d <= 7 + 3 * 11; d += 3) {
        mkt.expiries.push_back(mkt.asOf + d);
        mkt.atmVols.push_back(0.18);
    }
    const auto base = mkt.ssvi(-0.6);
    auto disordered = [&](double k, double T, std::size_t i) {
        return base(k, T, i) * (i % 3 == 2 ? 0.94 : 1.0);
    };
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(disordered, grid(0.85, 1.10, 0.0125), 0.01, mkt.expiries.size());
    in.minForwardVol = 0.02;

    auto off = in;
    off.enforceCalendar = false;
    const auto r0 = VolSurfaceCalibrationEvaluator().evaluate(off, mkt.reg, mkt.ctx);
    unsigned crossed = 0;
    for (std::size_t i = 1; i < r0.expiries.size(); ++i)
        if (r0.expiries[i].calendarMinMargin.value_or(1.0) < 0.0) ++crossed;
    EXPECT_GE(crossed, 3u);
    ASSERT_TRUE(r0.localVolCheckSvi.has_value());
    EXPECT_FALSE(r0.localVolCheckSvi->arbitrageFree);

    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    unsigned refits = 0;
    for (std::size_t i = 0; i < r.expiries.size(); ++i) {
        const auto& e = r.expiries[i];
        ASSERT_TRUE(e.usable) << e.droppedReason;
        if (e.calendarRefit) ++refits;
        if (i == 0) continue;
        ASSERT_TRUE(e.calendarMinMargin.has_value());
        EXPECT_GE(*e.calendarMinMargin, -1e-12) << e.expiryIso;
        EXPECT_GE(e.calendarMinForwardVol.value_or(-1.0), 0.02 - 1e-9) << e.expiryIso;
        EXPECT_EQ(e.calendarViolations, 0u) << e.expiryIso << ": " << e.calendarAction;
    }
    EXPECT_GE(refits, 3u);
    EXPECT_TRUE(r.localVolCheck.arbitrageFree) << r.localVolCheck.firstRefusal;
    ASSERT_TRUE(r.localVolCheckSvi.has_value());
    EXPECT_TRUE(r.localVolCheckSvi->arbitrageFree) << r.localVolCheckSvi->firstRefusal;
    EXPECT_EQ(r.sviLocalVolRefitAction, "none");
}

// The local vol check scans what it says: every expiry +/- 2e-4, the
// midpoints and T_last - 1e-3, 201 strikes per time; on the SSVI truth
// nothing is refused and the smallest local variance is positive.
TEST(VolSurfaceCalibration, LocalVolCheckScansTheReturnedSurfaces) {
    Market mkt;
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(mkt.ssvi(), kQuotedMoneyness);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    const unsigned n = static_cast<unsigned>(mkt.expiries.size());
    for (const auto* c : {&r.localVolCheck, &*r.localVolCheckSvi}) {
        EXPECT_EQ(c->times, 3 * n + 1);
        EXPECT_EQ(c->strikes, 201u);
        EXPECT_EQ(c->points, c->times * c->strikes);
        EXPECT_EQ(c->refused, 0u);
        EXPECT_TRUE(c->arbitrageFree);
        ASSERT_TRUE(c->minLocalVariance.has_value());
        EXPECT_GT(*c->minLocalVariance, 0.0);
    }
    EXPECT_EQ(r.sviLocalVolRefitAction, "none");
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
    EXPECT_LT(r.fit.inside, r.fit.count);

    in.enforceButterfly = false;
    const auto r2 = VolSurfaceCalibrationEvaluator().evaluate(in, one.reg, one.ctx);
    EXPECT_TRUE(r2.expiries[0].butterflyViolation);
    EXPECT_LT(*r2.expiries[0].butterflyMinG, -0.3);
    EXPECT_NE(r2.expiries[0].butterflyAction.find("reported only"), std::string::npos);
    EXPECT_NEAR(r2.expiries[0].svi->b, bad.b, 1e-4);
    EXPECT_EQ(r2.fit.inside, r2.fit.count);
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
    EXPECT_EQ(r.fit.count, fitted.fit.count);

    // SviPerExpiry: a grid expiry off the sheet is a 400; Andreasen-Huge
    // accepts it.
    in.gridExpiries = {QuantLib::Period(100, QuantLib::Days)};
    EXPECT_THROW(
        VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx), QuantraInvalidArgument);
    in.method = VolSurfaceFitMethod::AndreasenHuge;
    const auto rAh = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    ASSERT_EQ(rAh.gridExpiryIso, (std::vector<std::string>{"2027-01-08"}));
    // Andreasen-Huge returns its own finite-difference strikes inside the
    // output range (and the expiries' regions): read the one nearest the spot.
    std::size_t atm = 0;
    for (std::size_t j = 0; j < rAh.gridStrikes.size(); ++j)
        if (std::fabs(rAh.gridStrikes[j] - kSpot) < std::fabs(rAh.gridStrikes[atm] - kSpot))
            atm = j;
    EXPECT_GT(rAh.gridStrikes.size(), 5u);
    EXPECT_NEAR(rAh.gridStrikes[atm], kSpot, 0.01 * kSpot);
    EXPECT_GT(gridVol(rAh, 0, atm), 0.15);
    EXPECT_LT(gridVol(rAh, 0, atm), 0.30);
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

// ===========================================================================
// /validate-vol-surface: the one implementation of each test, shared with the
// calibrate endpoint.
// ===========================================================================

namespace {

/// The registry entry the parser builds from the returned payloads.
BlackVolEntry gridEntry(const VolSurfaceCalibrationResult& r, const Date& asOf) {
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    std::vector<Date> dates;
    for (int d : r.gridExpiryDays)
        dates.push_back(asOf + d);
    const std::size_t nStr = r.gridStrikes.size();
    std::vector<std::vector<double>> rows(dates.size(), std::vector<double>(nStr));
    for (std::size_t i = 0; i < dates.size(); ++i)
        for (std::size_t j = 0; j < nStr; ++j)
            rows[i][j] = r.gridVols[i * nStr + j];
    auto s = makeCubicStrikeLinearTimeSurface(
        asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, dates, r.gridStrikes, rows);
    s->enableExtrapolation();
    BlackVolEntry e;
    e.handle = Handle<QuantLib::BlackVolTermStructure>(s);
    e.shapeFb = quantra::enums::VolSurfaceShape_SmileCube3D;
    e.referenceDate = asOf;
    e.dayCounter = dc;
    e.pillarDates = dates;
    e.strikes = r.gridStrikes;
    e.flatLocalVolBeyondLastExpiry = true;
    e.allowExtrapolation = true;
    for (std::size_t i = 0; i < dates.size(); ++i)
        e.localVolStrikeBands.push_back(
            {dates[i], r.gridLocalVolStrikeMin[i], r.gridLocalVolStrikeMax[i]});
    return e;
}

BlackVolEntry sliceEntry(const VolSurfaceCalibrationResult& r, const Date& asOf, bool ssvi) {
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    std::vector<Date> dates;
    std::vector<SviSliceParams> svi;
    std::vector<SsviSliceParams> ss;
    std::vector<double> fwds;
    BlackVolEntry e;
    double lo = QL_MAX_REAL, hi = 0.0;
    for (const auto& x : r.expiries) {
        if (!x.usable) continue;
        dates.push_back(x.expiry);
        fwds.push_back(*x.forward);
        if (ssvi) {
            ss.push_back(*x.ssvi);
        } else {
            svi.push_back({*x.forward, x.svi->a, x.svi->b, x.svi->sigma, x.svi->rho, x.svi->m});
        }
        e.localVolStrikeBands.push_back({x.expiry, *x.strikeMin, *x.strikeMax});
        lo = std::min(lo, *x.strikeMin);
        hi = std::max(hi, *x.strikeMax);
    }
    std::shared_ptr<QuantLib::BlackVolTermStructure> s;
    if (ssvi) {
        s = makeSsviSurface(
            asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, dates, ss, fwds, r.spot);
    } else {
        s = makeSviSurface(asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, dates, svi);
    }
    s->enableExtrapolation();
    e.handle = Handle<QuantLib::BlackVolTermStructure>(s);
    e.shapeFb = ssvi ? quantra::enums::VolSurfaceShape_SsviSlices
                     : quantra::enums::VolSurfaceShape_SviSlices;
    e.referenceDate = asOf;
    e.dayCounter = dc;
    e.pillarDates = dates;
    e.localVolStrikeRange = std::make_pair(lo, hi);
    e.flatLocalVolBeyondLastExpiry = !ssvi;
    e.allowExtrapolation = true;
    return e;
}

VolSurfaceValidationInputs validationInputs(
    const VolSurfaceCalibrationInputs& cin, const VolSurfaceCalibrationResult& r) {
    VolSurfaceValidationInputs v;
    v.volatilityId = "NDX_VOL";
    v.underlyingId = "NDX";
    v.discountingCurveId = "USD_OIS";
    v.sheetId = cin.base.sheetId;
    v.sheetUnderlyingId = cin.base.underlyingId;
    v.quotes = cin.base.quotes;
    v.filter = cin.base.filter;
    v.forwards = r.forwards;
    return v;
}

void expectSameCheck(const LocalVolCheckData& a, const LocalVolCheckData& b) {
    EXPECT_EQ(a.arbitrageFree, b.arbitrageFree);
    EXPECT_EQ(a.times, b.times);
    EXPECT_EQ(a.strikes, b.strikes);
    EXPECT_EQ(a.points, b.points);
    EXPECT_EQ(a.refused, b.refused);
    EXPECT_EQ(a.firstRefusal, b.firstRefusal);
    ASSERT_EQ(a.minLocalVariance.has_value(), b.minLocalVariance.has_value());
    if (a.minLocalVariance) {
        EXPECT_EQ(*a.minLocalVariance, *b.minLocalVariance);
        EXPECT_EQ(*a.minLocalVarianceTime, *b.minLocalVarianceTime);
        EXPECT_EQ(*a.minLocalVarianceStrike, *b.minLocalVarianceStrike);
    }
}

} // namespace

// Band summary: cells, open ends, inclusive lower bounds, RMS and max over the
// quotes that carry a vol error; empty cells are not listed.
TEST(VolSurfaceValidation, BandSummaryArithmetic) {
    VolSurfaceBandEdges edges{{0.9, 1.1}, {0.5}};
    std::vector<BandPoint> pts{
        {0.85, 0.2, true, 0.01},         // m < 0.9, t < 0.5
        {0.90, 0.2, false, -0.02},       // [0.9, 1.1): lower bound inclusive
        {1.00, 0.2, true, 0.04},         // same cell
        {1.10, 0.7, true, std::nullopt}, // m >= 1.1, t >= 0.5, no vol error
        {1.20, 0.5, false, 0.03},        // t = 0.5 is in the upper bucket
    };
    const auto cells = summarizeBands(pts, edges);
    ASSERT_EQ(cells.size(), 3u);
    EXPECT_FALSE(cells[0].moneynessMin.has_value());
    EXPECT_EQ(*cells[0].moneynessMax, 0.9);
    EXPECT_FALSE(cells[0].timeMin.has_value());
    EXPECT_EQ(*cells[0].timeMax, 0.5);
    EXPECT_EQ(cells[0].count, 1u);
    EXPECT_EQ(cells[0].inside, 1u);
    EXPECT_DOUBLE_EQ(*cells[0].rmsVolError, 0.01);
    EXPECT_EQ(*cells[1].moneynessMin, 0.9);
    EXPECT_EQ(*cells[1].moneynessMax, 1.1);
    EXPECT_EQ(cells[1].count, 2u);
    EXPECT_EQ(cells[1].inside, 1u);
    EXPECT_DOUBLE_EQ(*cells[1].rmsVolError, std::sqrt((0.02 * 0.02 + 0.04 * 0.04) / 2.0));
    EXPECT_DOUBLE_EQ(*cells[1].maxAbsVolError, 0.04);
    EXPECT_EQ(*cells[2].moneynessMin, 1.1);
    EXPECT_FALSE(cells[2].moneynessMax.has_value());
    EXPECT_EQ(*cells[2].timeMin, 0.5);
    EXPECT_FALSE(cells[2].timeMax.has_value());
    EXPECT_EQ(cells[2].count, 2u);
    EXPECT_EQ(cells[2].inside, 1u);
    EXPECT_DOUBLE_EQ(*cells[2].rmsVolError, 0.03); // the quote without one is left out
    // Defaults and the 400s.
    const auto d = resolveBandEdges({}, "bands");
    EXPECT_EQ(d.moneyness, (std::vector<double>{0.8, 0.9, 0.95, 1.05, 1.1, 1.2}));
    EXPECT_EQ(d.maturities.size(), 5u);
    EXPECT_THROW(resolveBandEdges({{1.0, 0.9}, {}}, "bands"), QuantraInvalidArgument);
    EXPECT_THROW(resolveBandEdges({{}, {0.0}}, "bands"), QuantraInvalidArgument);
}

// The fit of a freshly calibrated grid equals the calibrate-time fit to the
// bit (same function, same forwards, same quotes, the surface rebuilt as the
// parser builds it); the local vol checks of the grid and the SVI payload
// equal the calibrate endpoint's own.
TEST(VolSurfaceValidation, FitAndArbitrageEqualTheCalibrateTimeNumbers) {
    Market mkt;
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(mkt.ssvi(), kQuotedMoneyness);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);

    PricingRegistry& reg = mkt.reg;
    reg.volatility.blackVols["NDX_VOL"] = gridEntry(r, mkt.asOf);
    const auto v = VolSurfaceValidationEvaluator().evaluate(validationInputs(in, r), reg, mkt.ctx);
    ASSERT_TRUE(v.fit.has_value());
    EXPECT_EQ(v.forwardsSource, "request");
    ASSERT_EQ(v.fit->quotes.size(), r.fit.quotes.size());
    EXPECT_GT(v.fit->count, 150u);
    for (std::size_t i = 0; i < r.fit.quotes.size(); ++i) {
        const auto &a = v.fit->quotes[i], &b = r.fit.quotes[i];
        EXPECT_EQ(a.expiryIso, b.expiryIso);
        EXPECT_EQ(a.strike, b.strike);
        EXPECT_EQ(a.marketVol, b.marketVol);
        EXPECT_EQ(a.modelVol, b.modelVol);
        EXPECT_EQ(a.modelPrice, b.modelPrice);
        EXPECT_EQ(a.insideSpread, b.insideSpread);
    }
    EXPECT_EQ(v.fit->count, r.fit.count);
    EXPECT_EQ(v.fit->inside, r.fit.inside);
    EXPECT_EQ(v.fit->rmsVolError, r.fit.rmsVolError);
    EXPECT_EQ(v.fit->verdict.rfind("pass", 0), 0u) << v.fit->verdict;
    ASSERT_EQ(v.fit->atm.size(), 8u);
    ASSERT_TRUE(v.arbitrage.has_value());
    expectSameCheck(v.arbitrage->localVol, r.localVolCheck);
    EXPECT_TRUE(v.arbitrage->arbitrageFree);

    reg.volatility.blackVols["NDX_VOL"] = sliceEntry(r, mkt.asOf, false);
    const auto vs = VolSurfaceValidationEvaluator().evaluate(validationInputs(in, r), reg, mkt.ctx);
    ASSERT_TRUE(vs.arbitrage.has_value());
    ASSERT_TRUE(r.localVolCheckSvi.has_value());
    expectSameCheck(vs.arbitrage->localVol, *r.localVolCheckSvi);
    ASSERT_TRUE(vs.arbitrage->sviSlices.has_value());
    ASSERT_EQ(vs.arbitrage->sviSlices->size(), 8u);
    for (const auto& c : *vs.arbitrage->sviSlices) {
        EXPECT_GE(c.butterflyMinG, 0.0) << c.expiryIso;
        EXPECT_LE(c.wingSlope, 2.0) << c.expiryIso;
        if (c.calendarMinDw) {
            EXPECT_GE(*c.calendarMinDw, 0.0) << c.expiryIso;
        }
    }
    EXPECT_TRUE(vs.arbitrage->violations.empty());
    EXPECT_TRUE(vs.arbitrage->arbitrageFree) << vs.arbitrage->verdict;
    // On the SVI payload the fit reads the slices themselves: inside too.
    EXPECT_EQ(vs.fit->inside, vs.fit->count);
}

TEST(VolSurfaceValidation, SsviArbitrageEqualsTheCalibrateTimeCheck) {
    Market mkt;
    auto in = baseInputs(VolSurfaceFitMethod::Ssvi);
    in.base.quotes = mkt.lines(mkt.ssvi(), kQuotedMoneyness);
    const auto r = VolSurfaceCalibrationEvaluator().evaluate(in, mkt.reg, mkt.ctx);
    ASSERT_TRUE(r.localVolCheckSsvi.has_value());
    PricingRegistry& reg = mkt.reg;
    reg.volatility.blackVols["NDX_VOL"] = sliceEntry(r, mkt.asOf, true);
    const auto v = VolSurfaceValidationEvaluator().evaluate(validationInputs(in, r), reg, mkt.ctx);
    ASSERT_TRUE(v.arbitrage.has_value());
    expectSameCheck(v.arbitrage->localVol, *r.localVolCheckSsvi);
    ASSERT_TRUE(v.arbitrage->ssvi.has_value());
    ASSERT_TRUE(r.ssviArbitrage.has_value());
    EXPECT_EQ(v.arbitrage->ssvi->violations, r.ssviArbitrage->violations);
    EXPECT_EQ(v.arbitrage->ssvi->scanMinG, r.ssviArbitrage->scanMinG);
    EXPECT_EQ(v.arbitrage->ssvi->scanMinDwDt, r.ssviArbitrage->scanMinDwDt);
    ASSERT_TRUE(v.arbitrage->ssviSlices.has_value());
    for (std::size_t i = 0; i < v.arbitrage->ssviSlices->size(); ++i) {
        const auto& c = (*v.arbitrage->ssviSlices)[i];
        const auto& e = r.expiries[i];
        EXPECT_EQ(c.marginButterflyPsi, *e.ssviMarginButterflyPsi);
        EXPECT_EQ(c.marginButterflyPsi2, *e.ssviMarginButterflyPsi2);
        EXPECT_EQ(c.marginCalendarTheta.has_value(), e.ssviMarginCalendarTheta.has_value());
    }
    EXPECT_TRUE(v.arbitrage->arbitrageFree) << v.arbitrage->verdict;
}

// Monte Carlo on a constant vol (exact lognormal steps, Mersenne Twister):
// every quote, the exact Black price with a 1 % bid/ask on the curves'
// forwards, is reproduced within 3.5 SE; the expiry report carries the
// curve forward and discount factor; the dropped lines carry their reasons;
// and the 400 rows.
TEST(VolSurfaceValidation, SimulationReproducesBlackOnAConstantVol) {
    Market mkt;
    const double vol = 0.25;
    PricingRegistry& reg = mkt.reg;
    auto dividend = std::make_shared<QuantLib::FlatForward>(mkt.asOf, kDiv, mkt.dc);
    reg.equity.equityUnderlyings["NDX"].dividend = Handle<QuantLib::YieldTermStructure>(dividend);
    BlackVolEntry e;
    e.handle = Handle<QuantLib::BlackVolTermStructure>(std::make_shared<QuantLib::BlackConstantVol>(
        mkt.asOf, QuantLib::NullCalendar(), vol, mkt.dc));
    e.constantVol = vol;
    e.shapeFb = quantra::enums::VolSurfaceShape_Constant;
    reg.volatility.blackVols["FLAT"] = e;
    auto in = baseInputs(VolSurfaceFitMethod::SviPerExpiry);
    in.base.quotes = mkt.lines(Market::flat(vol), grid(0.80, 1.20, 0.05), 0.01, 3);
    {
        OptionQuoteLine crossed = in.base.quotes.front();
        crossed.strike = 19000.0;
        crossed.callBid = *crossed.callAsk * 1.1; // crossed: dropped by the filter
        in.base.quotes.push_back(crossed);
    }
    VolSurfaceValidationInputs v;
    v.volatilityId = "FLAT";
    v.underlyingId = "NDX";
    v.discountingCurveId = "USD_OIS";
    v.sheetId = "NDX_TEST";
    v.sheetUnderlyingId = "NDX";
    v.quotes = in.base.quotes;
    v.checkSimulation = true;
    MonteCarloData mc;
    mc.paths = 1 << 15;
    mc.sequence = RandomSequenceKind::PseudoRandom;
    mc.brownianBridge = false;
    v.simulation = mc;
    const auto r = VolSurfaceValidationEvaluator().evaluate(v, reg, mkt.ctx);
    ASSERT_EQ(r.expiries.size(), 3u);
    unsigned used = 0;
    for (const auto& x : r.expiries) {
        ASSERT_TRUE(x.usable) << x.droppedReason;
        EXPECT_NEAR(*x.discountFactor, mkt.df(x.expiry), 1e-15);
        EXPECT_NEAR(*x.forward, mkt.forward(x.expiry), 1e-9);
        EXPECT_GE(x.used.size(), 6u);
        used += static_cast<unsigned>(x.used.size());
    }
    EXPECT_TRUE(
        std::any_of(
            r.expiries[0].dropped.begin(), r.expiries[0].dropped.end(),
            [](const DroppedQuoteData& d) {
                return d.reason.find("crossed") != std::string::npos;
            }));
    ASSERT_TRUE(r.fit && r.arbitrage && r.simulation);
    EXPECT_EQ(r.fit->inside, r.fit->count); // the exact surface
    EXPECT_TRUE(r.arbitrage->arbitrageFree);
    const auto& s = *r.simulation;
    EXPECT_EQ(s.count, used);
    EXPECT_EQ(s.timeSteps, 3u); // exact steps, no refinement
    EXPECT_EQ(s.paths, mc.paths);
    for (const auto& q : s.quotes) {
        ASSERT_GT(q.standardError, 0.0);
        EXPECT_LE(std::fabs(q.mcPrice - q.mid) / q.standardError, 3.5)
            << q.expiryIso << " " << q.strike;
        EXPECT_TRUE(q.withinNoise);
        ASSERT_TRUE(q.mcVol.has_value());
        EXPECT_NEAR(*q.volError, *q.mcVol - q.marketVol, 1e-15);
    }
    EXPECT_EQ(s.withinNoise, s.count);
    EXPECT_EQ(s.verdict.rfind("pass", 0), 0u) << s.verdict;
    ASSERT_TRUE(s.maxAbsVolError.has_value());
    EXPECT_LT(*s.maxAbsVolError, 0.01);

    auto expect400 = [&](const VolSurfaceValidationInputs& bad, const char* needle) {
        try {
            VolSurfaceValidationEvaluator().evaluate(bad, reg, mkt.ctx);
            FAIL() << "expected a 400 containing '" << needle << "'";
        } catch (const QuantraInvalidArgument& ex) {
            EXPECT_NE(std::string(ex.what()).find(needle), std::string::npos) << ex.what();
        }
    };
    {
        auto bad = v;
        bad.simulation.reset();
        expect400(bad, "simulation: required");
    }
    {
        auto bad = v;
        bad.checkSimulation = false;
        expect400(bad, "simulation: must be absent");
    }
    {
        auto bad = v;
        bad.simulation->stepsPerYear = 52;
        expect400(bad, "simulation.steps_per_year: must be absent");
    }
    {
        auto bad = v;
        bad.sheetUnderlyingId = "SPX";
        expect400(bad, "quotes.underlying_id 'SPX' must equal underlying_id 'NDX'");
    }
    {
        auto bad = v;
        bad.style = OptionSheetStyle::American;
        expect400(bad, "quotes.option_style must be European");
    }
    {
        auto bad = v;
        bad.bands.maturities = {1.0, 0.5};
        expect400(bad, "bands.maturities must be strictly increasing");
    }
    {
        auto bad = v;
        bad.checkFit = bad.checkArbitrage = bad.checkSimulation = false;
        bad.simulation.reset();
        expect400(bad, "checks: at least one");
    }
    {
        auto bad = v;
        bad.volatilityId = "NOPE";
        EXPECT_THROW(VolSurfaceValidationEvaluator().evaluate(bad, reg, mkt.ctx), QuantraNotFound);
    }
}

}} // namespace quantra::testing
