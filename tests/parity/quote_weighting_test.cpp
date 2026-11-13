// Quote weighting of the vol-surface fits (src/market/quote_weighting.h) and
// the weighted Andreasen-Huge calibration (src/market/weighted_andreasen_huge.h).
//
//   * the three weights against their formulas; the scaled Vega weights
//     against the SviPerExpiry formula of the versions before the option;
//     BidAsk: price and vol spreads, the median for a one-sided quote, the
//     quote left out, the 400 naming the expiry;
//   * WeightedAndreasenHuge with all weights zero against QuantLib's
//     AndreasenHugeVolatilityInterpl + adapter (same vols: the first stage
//     alone);
//   * with uniform, vega and inverse squared spread weights, on quotes the
//     model matches and on quotes it cannot match: the weighted cost of the
//     result is never above the first stage's, and the weighted vol error
//     at the quotes is never above that of QuantLib's class;
//   * on quotes no arbitrage-free model matches (a butterfly violation),
//     raising a quote's weight lowers its error.
#include "quote_weighting.h"

#include "error.h"
#include "weighted_andreasen_huge.h"

#include <ql/exercise.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/distributions/normaldistribution.hpp>
#include <ql/math/functional.hpp>
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

// QuantLib's class on the quotes of `expiries`.
std::shared_ptr<QuantLib::AndreasenHugeVolatilityInterpl> quantLibFit(
    const AhMarket& m, const std::vector<WeightedAndreasenHuge::Expiry>& expiries) {
    QuantLib::AndreasenHugeVolatilityInterpl::CalibrationSet set;
    for (const auto& x : expiries) {
        const auto exercise = std::make_shared<QuantLib::EuropeanExercise>(x.date);
        for (const auto& q : x.quotes) {
            const auto type =
                q.strike >= m.forward(x.date) ? QuantLib::Option::Call : QuantLib::Option::Put;
            set.emplace_back(
                std::make_shared<QuantLib::VanillaOption>(
                    std::make_shared<QuantLib::PlainVanillaPayoff>(type, q.strike), exercise),
                std::make_shared<QuantLib::SimpleQuote>(q.vol));
        }
    }
    const Handle<QuantLib::Quote> spotH(std::make_shared<QuantLib::SimpleQuote>(m.spot));
    return std::make_shared<QuantLib::AndreasenHugeVolatilityInterpl>(
        set, spotH, m.rTS, m.qTS, QuantLib::AndreasenHugeVolatilityInterpl::CubicSpline,
        QuantLib::AndreasenHugeVolatilityInterpl::CallPut, 500);
}

enum class TestWeights { Zero, Uniform, Vega, InverseSquaredSpread };

// The quotes of the market, the smile of AhMarket::vol or, with `kinked`,
// the same smile with every second strike 3 vol points higher (butterfly
// violations: no arbitrage-free model matches these quotes).
std::vector<WeightedAndreasenHuge::Expiry> ahQuotes(
    const AhMarket& m, TestWeights weights, bool kinked) {
    std::vector<WeightedAndreasenHuge::Expiry> expiries;
    for (const Date& d : m.expiries) {
        WeightedAndreasenHuge::Expiry x;
        x.date = d;
        for (std::size_t j = 0; j < m.strikes.size(); ++j) {
            const double K = m.strikes[j];
            const double v = m.vol(d, K) + ((kinked && j % 2 == 1) ? 0.03 : 0.0);
            double w = 0.0;
            if (weights == TestWeights::Uniform) w = 1.0;
            if (weights == TestWeights::Vega) w = m.forwardVega(d, K, v);
            // A vol spread that widens away from the money.
            if (weights == TestWeights::InverseSquaredSpread)
                w = 1.0 / QuantLib::squared(0.002 + 0.02 * std::fabs(std::log(K / m.forward(d))));
            x.quotes.push_back(WeightedAndreasenHuge::Quote{K, v, w});
        }
        expiries.push_back(x);
    }
    return expiries;
}

// sum_i weight_i (vol_i - market vol_i)^2 / sum_i weight_i at one expiry.
double weightedMeanSquaredVolError(
    const WeightedAndreasenHuge::Expiry& x, const std::function<double(double)>& volAt) {
    double num = 0.0, den = 0.0;
    for (const auto& q : x.quotes) {
        num += q.weight * QuantLib::squared(volAt(q.strike) - q.vol);
        den += q.weight;
    }
    return num / den;
}

} // namespace

TEST(WeightedAndreasenHuge, ZeroWeightsAreQuantLibsCalibration) {
    AhMarket m;
    for (const bool kinked : {false, true}) {
        const auto expiries = ahQuotes(m, TestWeights::Zero, kinked);
        const auto ql = quantLibFit(m, expiries);
        const QuantLib::AndreasenHugeVolatilityAdapter adapter(ql);
        const WeightedAndreasenHuge copy(expiries, m.spot, m.rTS, m.qTS);

        EXPECT_DOUBLE_EQ(copy.minStrike(), ql->minStrike());
        EXPECT_DOUBLE_EQ(copy.maxStrike(), ql->maxStrike());
        double maxDiff = 0.0, maxErr = 0.0;
        for (const Date& d : m.expiries) {
            EXPECT_DOUBLE_EQ(copy.fwd(m.time(d)), ql->fwd(m.time(d)));
            for (double K = 72.5; K <= 122.5; K += 2.5) {
                const double a = adapter.blackVol(d, K, true), b = copy.blackVol(d, K);
                maxDiff = std::max(maxDiff, std::fabs(a - b));
                const auto type =
                    K >= m.forward(d) ? QuantLib::Option::Call : QuantLib::Option::Put;
                EXPECT_NEAR(
                    copy.optionPrice(m.time(d), K, type), ql->optionPrice(m.time(d), K, type),
                    1e-13);
            }
            for (double K : m.strikes)
                maxErr = std::max(maxErr, std::fabs(copy.blackVol(d, K) - m.vol(d, K)));
        }
        EXPECT_LT(maxDiff, 1e-12) << "largest vol difference to QuantLib's class";
        if (!kinked) {
            EXPECT_LT(maxErr, 2e-3) << "largest vol error on the quotes";
        }
        // A date between two expiries.
        const Date mid(31, QuantLib::March, 2027);
        EXPECT_NEAR(adapter.blackVol(mid, 97.0, true), copy.blackVol(mid, 97.0), 1e-12);

        // The second stage has nothing to lower and does not run.
        EXPECT_FALSE(copy.weightedStageUsed());
        EXPECT_EQ(copy.priceStageCost(), 0.0);
        ASSERT_EQ(copy.fits().size(), m.expiries.size());
        for (const auto& f : copy.fits()) {
            EXPECT_GT(f.priceStageEvaluations, 0u);
            EXPECT_EQ(f.weightedStageEvaluations, 0u);
            EXPECT_FALSE(f.weightedStageKept);
        }
    }
}

TEST(WeightedAndreasenHuge, TheResultIsNeverWorseThanTheFirstStageInTheWeightedCost) {
    AhMarket m;
    for (const bool kinked : {false, true}) {
        for (const TestWeights weights :
             {TestWeights::Uniform, TestWeights::Vega, TestWeights::InverseSquaredSpread}) {
            SCOPED_TRACE(
                std::string(kinked ? "kinked" : "smooth") + " quotes, weights " +
                std::to_string(static_cast<int>(weights)));
            const auto expiries = ahQuotes(m, weights, kinked);
            const WeightedAndreasenHuge fit(expiries, m.spot, m.rTS, m.qTS);
            const QuantLib::AndreasenHugeVolatilityAdapter ql(quantLibFit(m, expiries));

            // The bookkeeping: the surface returned is the one with the
            // smaller weighted cost.
            ASSERT_EQ(fit.fits().size(), expiries.size());
            double priceStage = 0.0, weightedStage = 0.0, returned = 0.0;
            for (const auto& f : fit.fits()) {
                priceStage += f.priceStageCost;
                weightedStage += f.weightedStageCost;
                returned += f.cost;
                EXPECT_EQ(f.cost, fit.weightedStageUsed() ? f.weightedStageCost : f.priceStageCost);
            }
            EXPECT_DOUBLE_EQ(priceStage, fit.priceStageCost());
            EXPECT_DOUBLE_EQ(weightedStage, fit.weightedStageCost());
            EXPECT_EQ(fit.weightedStageUsed(), fit.weightedStageCost() < fit.priceStageCost());
            EXPECT_LE(returned, fit.priceStageCost());
            // Here the second stage lowers the cost and is returned.
            EXPECT_TRUE(fit.weightedStageUsed());
            EXPECT_LT(fit.weightedStageCost(), 0.9 * fit.priceStageCost());

            // The same from outside: the cost is the vol error blackVol()
            // returns at the quotes, and the first stage is QuantLib's class.
            double ours = 0.0, quantLibs = 0.0;
            for (std::size_t i = 0; i < expiries.size(); ++i) {
                const Date d = expiries[i].date;
                const double oursAtExpiry = weightedMeanSquaredVolError(
                    expiries[i], [&](double K) { return fit.blackVol(d, K); });
                const double quantLibsAtExpiry = weightedMeanSquaredVolError(
                    expiries[i], [&](double K) { return ql.blackVol(d, K, true); });
                // blackVol() inverts to 1e-6 in standard deviation.
                EXPECT_NEAR(std::sqrt(oursAtExpiry), std::sqrt(fit.fits()[i].cost), 1e-5);
                EXPECT_NEAR(
                    std::sqrt(quantLibsAtExpiry), std::sqrt(fit.fits()[i].priceStageCost), 1e-5);
                ours += oursAtExpiry;
                quantLibs += quantLibsAtExpiry;
            }
            RecordProperty("weighted_rms_vol_error", std::sqrt(ours / expiries.size()));
            RecordProperty(
                "weighted_rms_vol_error_quantlib", std::sqrt(quantLibs / expiries.size()));
            EXPECT_LT(ours, quantLibs);
            if (!kinked) {
                // Quotes the model matches: both fits are on the quotes.
                for (const auto& x : expiries)
                    for (const auto& q : x.quotes)
                        EXPECT_NEAR(fit.blackVol(x.date, q.strike), q.vol, 1e-4);
            }
        }
    }
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
