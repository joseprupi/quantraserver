// Quote weighting of the vol-surface fits (src/market/quote_weighting.h) and
// the weighted Andreasen-Huge calibration (src/market/weighted_andreasen_huge.h).
//
//   * the three weights against their formulas; the scaled Vega weights
//     against the SviPerExpiry formula of the versions before the option;
//     BidAsk: price and vol spreads, the median for a one-sided quote, the
//     quote left out, the 400 naming the expiry;
//   * WeightedAndreasenHuge with weight vega^2 and no vega floor against
//     QuantLib's AndreasenHugeVolatilityInterpl + adapter (same vols);
//   * on quotes no arbitrage-free model matches (a butterfly violation),
//     raising a quote's weight lowers its error.
#include "quote_weighting.h"

#include "error.h"
#include "weighted_andreasen_huge.h"

#include <ql/exercise.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/pricingengines/blackformula.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/settings.hpp>
#include <ql/termstructures/volatility/equityfx/andreasenhugevolatilityadapter.hpp>
#include <ql/termstructures/volatility/equityfx/andreasenhugevolatilityinterpl.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <cmath>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;

const double kF = 102.0, kD = 0.97, kT = 0.75;
const std::vector<double> kStrikes{80.0, 90.0, 100.0, 110.0, 125.0};
const std::vector<double> kVols{0.27, 0.23, 0.20, 0.19, 0.20};

double vegaOf(double K, double vol) {
    const double sd = vol * std::sqrt(kT);
    const double d1 = std::log(kF / K) / sd + 0.5 * sd;
    return kD * kF * std::sqrt(kT) * QuantLib::NormalDistribution()(d1);
}

std::string messageOf(const std::function<void()>& f) {
    try {
        f();
    } catch (const QuantraInvalidArgument& e) {
        return e.what();
    }
    return "";
}

} // namespace

TEST(QuoteWeighting, UniformAndVegaWeights) {
    ExpiryQuoteWeights uniform(QuoteWeighting::Uniform, kF, kD, kT);
    ExpiryQuoteWeights vega(QuoteWeighting::Vega, kF, kD, kT);
    for (std::size_t i = 0; i < kStrikes.size(); ++i) {
        EXPECT_TRUE(uniform.add(kStrikes[i], kVols[i], std::nullopt, std::nullopt, false));
        EXPECT_TRUE(vega.add(kStrikes[i], kVols[i], std::nullopt, std::nullopt, false));
    }
    const auto u = uniform.weights("2027-06-18");
    const auto un = uniform.normalizedWeights("2027-06-18");
    const auto v = vega.weights("2027-06-18");
    const auto vn = vega.normalizedWeights("2027-06-18");
    ASSERT_EQ(u.size(), kStrikes.size());
    ASSERT_EQ(v.size(), kStrikes.size());
    // The SviPerExpiry weights before the option: the Black vega on a unit
    // discount factor over the sum, accumulated in quote order.
    std::vector<double> old;
    double sum = 0.0;
    for (std::size_t i = 0; i < kStrikes.size(); ++i) {
        old.push_back(
            QuantLib::blackFormulaStdDevDerivative(kStrikes[i], kF, kVols[i] * std::sqrt(kT), 1.0));
        sum += old.back();
    }
    double total = 0.0;
    for (std::size_t i = 0; i < kStrikes.size(); ++i) {
        EXPECT_EQ(u[i], 1.0);
        EXPECT_DOUBLE_EQ(un[i], 1.0 / kStrikes.size());
        EXPECT_NEAR(v[i], vegaOf(kStrikes[i], kVols[i]), 1e-12 * v[i]);
        EXPECT_NEAR(v[i], blackVegaAtMarketVol(kStrikes[i], kF, kD, kT, kVols[i]), 1e-15);
        // Same bits: the default fit is the fit of the earlier versions.
        EXPECT_EQ(std::sqrt(vn[i]), std::sqrt(old[i] / sum));
        total += vn[i];
    }
    EXPECT_NEAR(total, 1.0, 1e-14);
    EXPECT_STREQ(quoteWeightingName(QuoteWeighting::Uniform), "Uniform");
    EXPECT_STREQ(quoteWeightingName(QuoteWeighting::Vega), "Vega");
    EXPECT_STREQ(quoteWeightingName(QuoteWeighting::BidAsk), "BidAsk");
}

TEST(QuoteWeighting, BidAskWeights) {
    // Vol spreads 0.01, 0.02, none, 0.04, 0.02: the one-sided quote takes
    // the median of the known spreads (0.02).
    const std::vector<std::optional<double>> spreads{0.01, 0.02, std::nullopt, 0.04, 0.02};
    ExpiryQuoteWeights w(QuoteWeighting::BidAsk, kF, kD, kT);
    for (std::size_t i = 0; i < kStrikes.size(); ++i) {
        std::optional<double> bid, ask = kVols[i] + 0.01;
        if (spreads[i]) {
            bid = kVols[i] - 0.5 * *spreads[i];
            ask = kVols[i] + 0.5 * *spreads[i];
        }
        EXPECT_TRUE(w.add(kStrikes[i], kVols[i], bid, ask, false));
    }
    const auto x = w.weights("2027-06-18");
    const std::vector<double> expected{1e4, 2500.0, 2500.0, 625.0, 2500.0};
    double sum = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i) {
        EXPECT_NEAR(x[i], expected[i], 1e-6 * expected[i]);
        sum += x[i];
    }
    const auto xn = w.normalizedWeights("2027-06-18");
    for (std::size_t i = 0; i < x.size(); ++i)
        EXPECT_NEAR(xn[i], x[i] / sum, 1e-14);

    // Price quotes: the spread is the price spread over the Black vega.
    ExpiryQuoteWeights p(QuoteWeighting::BidAsk, kF, kD, kT);
    EXPECT_TRUE(p.add(100.0, 0.20, 5.00, 5.30, true));
    const double volSpread = 0.30 / vegaOf(100.0, 0.20);
    EXPECT_NEAR(p.weights("2027-06-18")[0], 1.0 / (volSpread * volSpread), 1e-6 / volSpread);
    // The same spread through volBidAsk.
    const auto sides = volBidAsk(100.0, kF, kD, kT, 0.20, 5.00, 5.15, 5.30);
    ASSERT_TRUE(sides.has_value());
    EXPECT_NEAR(sides->ask - sides->bid, volSpread, 1e-12);
    EXPECT_NEAR(0.5 * (sides->ask + sides->bid), 0.20, 1e-12);

    // Ask not above bid: left out under BidAsk only.
    ExpiryQuoteWeights z(QuoteWeighting::BidAsk, kF, kD, kT);
    EXPECT_FALSE(z.add(100.0, 0.20, 0.20, 0.20, false));
    EXPECT_FALSE(z.add(100.0, 0.20, 0.21, 0.20, false));
    EXPECT_EQ(z.size(), 0u);
    ExpiryQuoteWeights zv(QuoteWeighting::Vega, kF, kD, kT);
    EXPECT_TRUE(zv.add(100.0, 0.20, 0.20, 0.20, false));
    EXPECT_STREQ(kAskNotAboveBid, "ask_not_above_bid");
}

TEST(QuoteWeighting, BidAskWithoutATwoSidedQuoteIsAClientErrorNamingTheExpiry) {
    ExpiryQuoteWeights w(QuoteWeighting::BidAsk, kF, kD, kT);
    for (std::size_t i = 0; i < kStrikes.size(); ++i)
        EXPECT_TRUE(w.add(kStrikes[i], kVols[i], std::nullopt, kVols[i] + 0.01, false));
    const std::string m = messageOf([&] { w.weights("2027-06-18"); });
    EXPECT_NE(m.find("weighting BidAsk needs bid and ask"), std::string::npos) << m;
    EXPECT_NE(m.find("2027-06-18"), std::string::npos) << m;
    EXPECT_THROW(w.normalizedWeights("2027-06-18"), QuantraInvalidArgument);
    // The other weightings need no bid or ask.
    ExpiryQuoteWeights v(QuoteWeighting::Vega, kF, kD, kT);
    EXPECT_TRUE(v.add(100.0, 0.20, std::nullopt, std::nullopt, false));
    EXPECT_NO_THROW(v.weights("2027-06-18"));
}

// ---------------------------------------------------------------------------
// Weighted Andreasen-Huge
// ---------------------------------------------------------------------------

namespace {

struct AhMarket {
    Date asOf{30, QuantLib::September, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    double spot = 100.0;
    Handle<QuantLib::YieldTermStructure> rTS, qTS;
    std::vector<Date> expiries{
        Date(30, QuantLib::December, 2026), Date(30, QuantLib::June, 2027),
        Date(30, QuantLib::December, 2027)};
    std::vector<double> strikes{75.0, 80.0, 85.0, 90.0, 95.0, 100.0, 105.0, 110.0, 115.0, 120.0};

    AhMarket() {
        QuantLib::Settings::instance().evaluationDate() = asOf;
        rTS = Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(asOf, 0.03, dc));
        qTS = Handle<QuantLib::YieldTermStructure>(
            std::make_shared<QuantLib::FlatForward>(asOf, 0.01, dc));
    }
    double time(const Date& d) const { return dc.yearFraction(asOf, d); }
    double forward(const Date& d) const { return spot * qTS->discount(d) / rTS->discount(d); }
    // A skewed smile, flatter with maturity.
    double vol(const Date& d, double K) const {
        const double k = std::log(K / forward(d));
        return 0.20 - 0.25 * k / std::sqrt(1.0 + 4.0 * time(d)) + 0.30 * k * k;
    }
    double forwardVega(const Date& d, double K, double v) const {
        const double T = time(d), df = rTS->discount(d), F = forward(d);
        const QuantLib::BlackCalculator c(QuantLib::Option::Put, K, F, v * std::sqrt(T), df);
        return c.vega(T) / (df * F);
    }
};

} // namespace

TEST(WeightedAndreasenHuge, VegaSquaredWeightsAreQuantLibsCalibration) {
    AhMarket m;
    QuantLib::AndreasenHugeVolatilityInterpl::CalibrationSet set;
    std::vector<WeightedAndreasenHuge::Expiry> expiries;
    for (const Date& d : m.expiries) {
        const auto exercise = std::make_shared<QuantLib::EuropeanExercise>(d);
        WeightedAndreasenHuge::Expiry x;
        x.date = d;
        for (double K : m.strikes) {
            const double v = m.vol(d, K);
            const auto type = K >= m.forward(d) ? QuantLib::Option::Call : QuantLib::Option::Put;
            set.emplace_back(
                std::make_shared<QuantLib::VanillaOption>(
                    std::make_shared<QuantLib::PlainVanillaPayoff>(type, K), exercise),
                std::make_shared<QuantLib::SimpleQuote>(v));
            const double vega = m.forwardVega(d, K, v);
            x.quotes.push_back(WeightedAndreasenHuge::Quote{K, v, vega * vega});
        }
        expiries.push_back(x);
    }
    const Handle<QuantLib::Quote> spotH(std::make_shared<QuantLib::SimpleQuote>(m.spot));
    const auto ql = std::make_shared<QuantLib::AndreasenHugeVolatilityInterpl>(
        set, spotH, m.rTS, m.qTS, QuantLib::AndreasenHugeVolatilityInterpl::CubicSpline,
        QuantLib::AndreasenHugeVolatilityInterpl::CallPut, 500);
    const QuantLib::AndreasenHugeVolatilityAdapter adapter(ql);
    const WeightedAndreasenHuge copy(expiries, m.spot, m.rTS, m.qTS, 500, 0.0);

    EXPECT_DOUBLE_EQ(copy.minStrike(), ql->minStrike());
    EXPECT_DOUBLE_EQ(copy.maxStrike(), ql->maxStrike());
    double maxDiff = 0.0, maxErr = 0.0;
    for (const Date& d : m.expiries) {
        EXPECT_DOUBLE_EQ(copy.fwd(m.time(d)), ql->fwd(m.time(d)));
        for (double K = 72.5; K <= 122.5; K += 2.5) {
            const double a = adapter.blackVol(d, K, true), b = copy.blackVol(d, K);
            maxDiff = std::max(maxDiff, std::fabs(a - b));
            const auto type = K >= m.forward(d) ? QuantLib::Option::Call : QuantLib::Option::Put;
            EXPECT_NEAR(
                copy.optionPrice(m.time(d), K, type), ql->optionPrice(m.time(d), K, type), 1e-13);
        }
        for (double K : m.strikes)
            maxErr = std::max(maxErr, std::fabs(copy.blackVol(d, K) - m.vol(d, K)));
    }
    EXPECT_LT(maxDiff, 1e-12) << "largest vol difference to QuantLib's class";
    EXPECT_LT(maxErr, 2e-3) << "largest vol error on the quotes";
    // A date between two expiries.
    const Date mid(31, QuantLib::March, 2027);
    EXPECT_NEAR(adapter.blackVol(mid, 97.0, true), copy.blackVol(mid, 97.0), 1e-12);
    ASSERT_EQ(copy.fits().size(), m.expiries.size());
    for (const auto& f : copy.fits())
        EXPECT_GT(f.functionEvaluations, 0u);
}

TEST(WeightedAndreasenHuge, RaisingAQuotesWeightLowersItsError) {
    AhMarket m;
    // One expiry; the 100 strike quoted 6 vol points above its neighbours:
    // its price is above the chord of the 95 and 105 prices (a butterfly
    // violation), so no arbitrage-free model matches the three quotes.
    const Date d = m.expiries[1];
    const std::size_t odd = 5;
    auto fit = [&](double oddWeight) {
        WeightedAndreasenHuge::Expiry x;
        x.date = d;
        for (std::size_t j = 0; j < m.strikes.size(); ++j)
            x.quotes.push_back(
                WeightedAndreasenHuge::Quote{
                    m.strikes[j], j == odd ? 0.26 : 0.20, j == odd ? oddWeight : 1.0});
        return std::make_shared<WeightedAndreasenHuge>(
            std::vector<WeightedAndreasenHuge::Expiry>{x}, m.spot, m.rTS, m.qTS);
    };
    auto errors = [&](const WeightedAndreasenHuge& ah, double& atOdd, double& neighbours) {
        atOdd = std::fabs(ah.blackVol(d, m.strikes[odd]) - 0.26);
        neighbours = std::max(
            std::fabs(ah.blackVol(d, m.strikes[odd - 1]) - 0.20),
            std::fabs(ah.blackVol(d, m.strikes[odd + 1]) - 0.20));
    };
    double oddHeavy, nbHeavy, oddLight, nbLight, oddEven, nbEven;
    errors(*fit(100.0), oddHeavy, nbHeavy);
    errors(*fit(1.0), oddEven, nbEven);
    errors(*fit(0.01), oddLight, nbLight);
    RecordProperty("odd_quote_error_weight_100", oddHeavy);
    RecordProperty("odd_quote_error_weight_1", oddEven);
    RecordProperty("odd_quote_error_weight_0_01", oddLight);
    // Not an exact fit under any weight.
    EXPECT_GT(oddEven + nbEven, 5e-3);
    // The quote's error falls as its weight rises, its neighbours' rises.
    EXPECT_LT(oddHeavy, oddEven);
    EXPECT_LT(oddEven, oddLight);
    EXPECT_GT(nbHeavy, nbEven);
    EXPECT_GT(nbEven, nbLight);
    EXPECT_LT(oddHeavy, 0.5 * oddLight);

    // Invalid weights are refused.
    WeightedAndreasenHuge::Expiry bad;
    bad.date = d;
    bad.quotes = {{90.0, 0.2, 1.0}, {100.0, 0.2, -1.0}, {110.0, 0.2, 1.0}};
    EXPECT_THROW(
        WeightedAndreasenHuge(
            std::vector<WeightedAndreasenHuge::Expiry>{bad}, m.spot, m.rTS, m.qTS),
        QuantLib::Error);
}

}} // namespace quantra::testing
