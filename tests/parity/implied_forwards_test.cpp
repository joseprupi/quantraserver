// Implied forwards and dividends from option quotes: evaluator tests.
//
// The quotes are generated here from known inputs (spot, flat continuous
// rate, a dividend yield per expiry, a Black smile) with QuantLib's
// blackFormula, quoted with a 1 % bid/ask around the exact mid, so put-call
// parity holds exactly and the fit must give the inputs back to floating
// point. Then the filters: noise, a thin expiry, a crossed line, far
// strikes, open interest, and the 400s (no survivor, non-European sheet).
#include "date_convert.h"
#include "error.h"
#include "implied_forwards_evaluator.h"

#include <ql/math/randomnumbers/mt19937uniformrng.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <memory>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;

constexpr double kSpot = 24000.0;
constexpr double kRate = 0.04;

struct SheetMarket {
    Date asOf{30, QuantLib::September, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    PricingRegistry reg;
    PricingContext ctx;
    std::shared_ptr<QuantLib::YieldTermStructure> discount;

    SheetMarket() {
        QuantLib::Settings::instance().evaluationDate() = asOf;
        ctx.asOf = asOf;
        discount = std::make_shared<QuantLib::FlatForward>(asOf, kRate, dc);
        reg.rates.curves["USD_OIS"] =
            std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>(discount);
        auto placeholder = std::make_shared<QuantLib::FlatForward>(asOf, 0.0, dc);
        reg.rates.curves["NDX_DIV_PLACEHOLDER"] =
            std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>(placeholder);
        EquityUnderlyingRuntime u;
        u.currency = "USD";
        u.spot = Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(kSpot));
        u.dividend = Handle<QuantLib::YieldTermStructure>(placeholder);
        reg.equity.equityUnderlyings["NDX"] = u;
    }

    double t(const Date& d) const { return dc.yearFraction(asOf, d); }
    double df(const Date& d) const { return discount->discount(d); }
    double forward(const Date& d, double q) const { return kSpot * std::exp((kRate - q) * t(d)); }

    // Exact Black mids with a relative bid/ask `spread`, optionally perturbed
    // multiplicatively by `noise` (uniform in [-noise, noise], seeded).
    std::vector<OptionQuoteLine> lines(
        const Date& expiry,
        double q,
        const std::vector<double>& strikes,
        double spread = 0.01,
        double noise = 0.0,
        unsigned long seed = 42,
        double openInterest = 1000.0) const {
        QuantLib::MersenneTwisterUniformRng rng(seed);
        const double T = t(expiry), F = forward(expiry, q), DF = df(expiry);
        std::vector<OptionQuoteLine> out;
        for (double K : strikes) {
            const double k = std::log(K / F);
            const double vol = std::min(1.0, std::max(0.05, 0.20 - 0.4 * k + 0.8 * k * k));
            const double sd = vol * std::sqrt(T);
            double c = QuantLib::blackFormula(QuantLib::Option::Call, K, F, sd, DF);
            double p = QuantLib::blackFormula(QuantLib::Option::Put, K, F, sd, DF);
            if (noise > 0.0) {
                c *= 1.0 + noise * (2.0 * rng.next().value - 1.0);
                p *= 1.0 + noise * (2.0 * rng.next().value - 1.0);
            }
            OptionQuoteLine l;
            l.expiry = expiry;
            l.expiryIso = DateToIso(expiry);
            l.strike = K;
            l.callBid = c * (1.0 - spread / 2);
            l.callAsk = c * (1.0 + spread / 2);
            l.putBid = p * (1.0 - spread / 2);
            l.putAsk = p * (1.0 + spread / 2);
            l.callOpenInterest = openInterest;
            l.putOpenInterest = openInterest;
            out.push_back(l);
        }
        return out;
    }
};

std::vector<double> grid(double lo, double hi, double step) {
    std::vector<double> ks;
    for (double m = lo; m <= hi + 1e-12; m += step)
        ks.push_back(std::round(kSpot * m));
    return ks;
}

ImpliedForwardsInputs baseInputs() {
    ImpliedForwardsInputs in;
    in.sheetId = "NDX_TEST";
    in.underlyingId = "NDX";
    in.discountingCurveId = "USD_OIS";
    in.dividendCurveId = "NDX_DIV_IMPLIED";
    return in;
}

void append(ImpliedForwardsInputs& in, const std::vector<OptionQuoteLine>& l) {
    in.quotes.insert(in.quotes.end(), l.begin(), l.end());
}

const std::vector<std::pair<Date, double>> kExpiries = {
    {Date(16, QuantLib::October, 2026), 0.008},
    {Date(18, QuantLib::December, 2026), 0.0095},
    {Date(18, QuantLib::June, 2027), 0.011},
    {Date(17, QuantLib::December, 2027), 0.012},
};

} // namespace

class ImpliedForwardsTest : public ::testing::Test {
protected:
    SheetMarket m;
};

TEST_F(ImpliedForwardsTest, ExactQuotesRecoverForwardDiscountFactorAndYield) {
    auto in = baseInputs();
    // 17 strikes from 80 % to 120 % of spot: all inside +/-25 % of every forward.
    const auto strikes = grid(0.80, 1.20, 0.025);
    for (const auto& [d, q] : kExpiries)
        append(in, m.lines(d, q, strikes));

    auto out = ImpliedForwardsEvaluator().evaluate(in, m.reg, m.ctx);
    EXPECT_EQ(out.sheetId, "NDX_TEST");
    EXPECT_EQ(out.spot, kSpot);
    ASSERT_EQ(out.expiries.size(), kExpiries.size());
    for (std::size_t i = 0; i < kExpiries.size(); ++i) {
        const auto& e = out.expiries[i];
        const auto& [d, q] = kExpiries[i];
        ASSERT_TRUE(e.usable) << e.droppedReason;
        EXPECT_EQ(e.expiry, d);
        EXPECT_EQ(e.quotesUsed, 17u);
        EXPECT_TRUE(e.dropped.empty());
        EXPECT_NEAR(e.time, m.t(d), 1e-15);
        EXPECT_NEAR(*e.forward / m.forward(d, q), 1.0, 1e-10);
        EXPECT_NEAR(*e.impliedDiscountFactor / m.df(d), 1.0, 1e-10);
        EXPECT_NEAR(*e.curveDiscountFactor, m.df(d), 1e-15);
        EXPECT_NEAR(*e.impliedDividendYield, q, 1e-10);
        EXPECT_NEAR(*e.impliedFundingBp, 0.0, 1e-6);
        EXPECT_LT(*e.fitResidualRmse, 1e-8);
        EXPECT_LT(*e.maxAbsResidual, 1e-8);
    }
    // Dividend curve: as-of node (first yield) + one per expiry.
    ASSERT_EQ(out.dividendCurve.size(), kExpiries.size() + 1);
    EXPECT_EQ(out.dividendCurve[0].dateIso, "2026-09-30");
    EXPECT_NEAR(out.dividendCurve[0].zeroRate, kExpiries[0].second, 1e-10);
    for (std::size_t i = 0; i < kExpiries.size(); ++i) {
        EXPECT_EQ(out.dividendCurve[i + 1].dateIso, DateToIso(kExpiries[i].first));
        EXPECT_NEAR(out.dividendCurve[i + 1].zeroRate, kExpiries[i].second, 1e-10);
    }
    EXPECT_EQ(out.dividendCurveId, "NDX_DIV_IMPLIED");
}

TEST_F(ImpliedForwardsTest, FarStrikesAreDroppedByTheMoneynessWindowAndTheFitStaysExact) {
    auto in = baseInputs();
    auto strikes = grid(0.80, 1.20, 0.05);
    strikes.push_back(std::round(kSpot * 0.60));
    strikes.push_back(std::round(kSpot * 1.50));
    const auto& [d, q] = kExpiries[2];
    append(in, m.lines(d, q, strikes));
    in.filter.moneynessWindow = 0.25;

    auto out = ImpliedForwardsEvaluator().evaluate(in, m.reg, m.ctx);
    ASSERT_EQ(out.expiries.size(), 1u);
    const auto& e = out.expiries[0];
    ASSERT_TRUE(e.usable) << e.droppedReason;
    EXPECT_EQ(e.quotesUsed, 9u);
    ASSERT_EQ(e.dropped.size(), 2u);
    for (const auto& dr : e.dropped) {
        EXPECT_EQ(dr.reason.rfind("outside moneyness window", 0), 0u) << dr.reason;
        ASSERT_TRUE(dr.strike.has_value());
        EXPECT_TRUE(*dr.strike < 0.75 * kSpot || *dr.strike > 1.25 * kSpot);
    }
    EXPECT_NEAR(*e.forward / m.forward(d, q), 1.0, 1e-10);
}

TEST_F(ImpliedForwardsTest, NoisyQuotesRecoverWithinTheNoise) {
    auto in = baseInputs();
    const auto strikes = grid(0.80, 1.20, 0.025);
    for (const auto& [d, q] : kExpiries)
        append(in, m.lines(d, q, strikes, 0.01, 0.02));

    auto out = ImpliedForwardsEvaluator().evaluate(in, m.reg, m.ctx);
    ASSERT_EQ(out.expiries.size(), kExpiries.size());
    for (std::size_t i = 0; i < kExpiries.size(); ++i) {
        const auto& e = out.expiries[i];
        const auto& [d, q] = kExpiries[i];
        ASSERT_TRUE(e.usable) << e.droppedReason;
        EXPECT_LT(std::fabs(*e.forward / m.forward(d, q) - 1.0), 0.01);
        EXPECT_GT(*e.fitResidualRmse, 0.0);
        EXPECT_GE(*e.maxAbsResidual, *e.fitResidualRmse);
        EXPECT_TRUE(std::isfinite(*e.impliedDividendYield));
    }
}

TEST_F(ImpliedForwardsTest, ThreePairsExpiryIsDroppedAndReportedNotRejected) {
    auto in = baseInputs();
    append(in, m.lines(kExpiries[0].first, kExpiries[0].second, grid(0.9, 1.1, 0.025)));
    append(in, m.lines(kExpiries[1].first, kExpiries[1].second, {23000.0, 24000.0, 25000.0}));

    auto out = ImpliedForwardsEvaluator().evaluate(in, m.reg, m.ctx);
    ASSERT_EQ(out.expiries.size(), 2u);
    EXPECT_TRUE(out.expiries[0].usable);
    const auto& thin = out.expiries[1];
    EXPECT_FALSE(thin.usable);
    EXPECT_FALSE(thin.forward.has_value());
    EXPECT_FALSE(thin.impliedDividendYield.has_value());
    EXPECT_NE(thin.droppedReason.find("min_quotes_per_expiry (4)"), std::string::npos);
    EXPECT_NE(thin.droppedReason.find(": 3"), std::string::npos);
    ASSERT_EQ(thin.dropped.size(), 1u);
    EXPECT_FALSE(thin.dropped[0].strike.has_value());
    EXPECT_EQ(thin.dropped[0].reason, thin.droppedReason);
    EXPECT_EQ(out.dividendCurve.size(), 2u);

    in.filter.minQuotesPerExpiry = 3;
    out = ImpliedForwardsEvaluator().evaluate(in, m.reg, m.ctx);
    EXPECT_TRUE(out.expiries[1].usable);
    EXPECT_NEAR(
        *out.expiries[1].forward / m.forward(kExpiries[1].first, kExpiries[1].second), 1.0, 1e-10);
}

TEST_F(ImpliedForwardsTest, CrossedAndMalformedLinesAreDroppedWithReasons) {
    auto in = baseInputs();
    auto l = m.lines(kExpiries[2].first, kExpiries[2].second, grid(0.8, 1.2, 0.025));
    std::swap(l[3].callBid, l[3].callAsk); // crossed call
    l[4].putBid = std::nullopt;            // missing put side
    l[5].strike = -1.0;                    // bad strike
    l[6].callAsk = 0.0;                    // non-positive ask
    l[7] = l[8];                           // duplicate line (same strike)
    l[9].callOpenInterest = 10.0;          // thin
    append(in, l);
    in.filter.minOpenInterest = 100.0;

    auto out = ImpliedForwardsEvaluator().evaluate(in, m.reg, m.ctx);
    const auto& e = out.expiries[0];
    ASSERT_TRUE(e.usable) << e.droppedReason;
    EXPECT_EQ(e.quotesUsed, 17u - 6u);
    ASSERT_EQ(e.dropped.size(), 6u);
    auto reason = [&](double strike) {
        for (const auto& d : e.dropped)
            if (d.strike && *d.strike == strike) return d.reason;
        return std::string("<none>");
    };
    EXPECT_EQ(reason(*l[3].strike), "crossed quote (bid > ask)");
    EXPECT_EQ(reason(*l[4].strike), "missing call or put side");
    EXPECT_EQ(reason(-1.0), "strike missing or not > 0");
    EXPECT_EQ(reason(*l[6].strike), "negative bid or non-positive ask");
    EXPECT_EQ(reason(*l[9].strike), "open interest below min_open_interest");
    unsigned duplicates = 0;
    for (const auto& d : e.dropped)
        duplicates += d.reason == "duplicate strike";
    EXPECT_EQ(duplicates, 1u);
    EXPECT_NEAR(*e.forward / m.forward(kExpiries[2].first, kExpiries[2].second), 1.0, 1e-10);
}

TEST_F(ImpliedForwardsTest, ZeroSpreadFallsBackToEqualWeights) {
    auto in = baseInputs();
    append(in, m.lines(kExpiries[1].first, kExpiries[1].second, grid(0.9, 1.1, 0.025), 0.0));
    auto out = ImpliedForwardsEvaluator().evaluate(in, m.reg, m.ctx);
    ASSERT_TRUE(out.expiries[0].usable) << out.expiries[0].droppedReason;
    EXPECT_NEAR(
        *out.expiries[0].forward / m.forward(kExpiries[1].first, kExpiries[1].second), 1.0, 1e-10);
}

TEST_F(ImpliedForwardsTest, NoSurvivingExpiryIs400ListingEveryReason) {
    auto in = baseInputs();
    append(in, m.lines(kExpiries[0].first, kExpiries[0].second, {23000.0, 24000.0}));
    append(in, m.lines(kExpiries[3].first, kExpiries[3].second, {23000.0, 24000.0, 25000.0}));
    try {
        ImpliedForwardsEvaluator().evaluate(in, m.reg, m.ctx);
        FAIL() << "expected QuantraInvalidArgument";
    } catch (const QuantraInvalidArgument& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("No expiry"), std::string::npos) << msg;
        EXPECT_NE(msg.find("2026-10-16"), std::string::npos) << msg;
        EXPECT_NE(msg.find("2027-12-17"), std::string::npos) << msg;
        EXPECT_NE(msg.find("min_quotes_per_expiry"), std::string::npos) << msg;
    }
}

TEST_F(ImpliedForwardsTest, NonEuropeanSheetAndBadFilterAre400s) {
    auto in = baseInputs();
    append(in, m.lines(kExpiries[0].first, kExpiries[0].second, grid(0.9, 1.1, 0.025)));
    auto expect400 = [&](ImpliedForwardsInputs bad, const char* needle) {
        try {
            ImpliedForwardsEvaluator().evaluate(bad, m.reg, m.ctx);
            FAIL() << "expected QuantraInvalidArgument for " << needle;
        } catch (const QuantraInvalidArgument& e) {
            EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
        }
    };
    auto american = in;
    american.style = OptionSheetStyle::American;
    expect400(american, "option_style");
    auto spread = in;
    spread.filter.maxRelativeSpread = 0.0;
    expect400(spread, "max_relative_spread");
    auto minq = in;
    minq.filter.minQuotesPerExpiry = 1;
    expect400(minq, "min_quotes_per_expiry");
    auto window = in;
    window.filter.moneynessWindow = 0.0;
    expect400(window, "moneyness_window");
    auto empty = in;
    empty.quotes.clear();
    expect400(empty, "sheet.quotes");

    auto noCurve = in;
    noCurve.discountingCurveId = "nope";
    EXPECT_THROW(ImpliedForwardsEvaluator().evaluate(noCurve, m.reg, m.ctx), QuantraNotFound);
    auto noUnderlying = in;
    noUnderlying.underlyingId = "nope";
    EXPECT_THROW(ImpliedForwardsEvaluator().evaluate(noUnderlying, m.reg, m.ctx), QuantraNotFound);
}

}} // namespace quantra::testing
