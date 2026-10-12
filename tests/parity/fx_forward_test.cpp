// FX forward parity and contract tests.
//
// Parity: the endpoint's npv, fair forward, points and leg PVs must equal the
// discount-factor algebra the product documents (fx_forward_evaluator.h),
// evaluated here on QuantLib curves built independently of the engine (the
// same deposit/swap helper set as parity_fixture.h, one curve per currency at
// its own flat rate): rates to 1e-12 relative, amounts to 1e-12 of the
// notional (bootstrap accuracy). Direction, reporting currency, the
// K = fair round trip and the spot/maturity date conventions are pinned.
//
// Contract: the mapper's validations are 400s naming the field, the pair
// must be the spot's pair in the same order (never inverted), unknown
// references are 404s.
#include "fx_forward_handler.h"
#include "parity_fixture.h"

#include "fx_forward_response_generated.h"
#include "price_fx_forward_request_generated.h"

#include <ql/currencies/europe.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/time/calendars/jointcalendar.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedstates.hpp>

#include <cmath>
#include <functional>

namespace quantra { namespace testing {

namespace {

constexpr double kSpot = 1.0850;
constexpr double kNotional = 10000000.0;
constexpr double kEurRate = 0.03; // = the fixture's flatRate_ (discountHandle_)
constexpr double kUsdRate = 0.045;

const std::vector<int8_t> kSettlementCalendars = {
    static_cast<int8_t>(quantra::enums::Calendar_TARGET),
    static_cast<int8_t>(quantra::enums::Calendar_UnitedStatesSettlement)};

QuantLib::Calendar jointCalendar() {
    return QuantLib::JointCalendar(
        QuantLib::TARGET(), QuantLib::UnitedStates(QuantLib::UnitedStates::Settlement),
        QuantLib::JoinHolidays);
}

/// Independent QuantLib curve with the fixture's helper set at a flat rate
/// (mirrors QuantraComparisonTest::buildBootstrappedCurve, parameterised).
std::shared_ptr<QuantLib::YieldTermStructure> referenceCurve(
    double rate, const QuantLib::Date& asOf) {
    std::vector<std::shared_ptr<QuantLib::RateHelper>> instruments;
    for (const auto& tenor : {3 * QuantLib::Months, 6 * QuantLib::Months, 1 * QuantLib::Years}) {
        instruments.push_back(
            std::make_shared<QuantLib::DepositRateHelper>(
                rate, tenor, 2, QuantLib::TARGET(), QuantLib::ModifiedFollowing, true,
                QuantLib::Actual365Fixed()));
    }
    // The fixture's EUR_6M IndexDef (Euribor-like, end_of_month false), not
    // QuantLib's Euribor6M whose end-of-month rule is true: the swap helpers'
    // schedules must match the engine's for the curves to agree past 1Y.
    auto euribor6m = std::make_shared<QuantLib::IborIndex>(
        "Euribor", 6 * QuantLib::Months, 2, QuantLib::EURCurrency(), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, false, QuantLib::Actual360());
    for (const auto& tenor : {5 * QuantLib::Years, 10 * QuantLib::Years}) {
        instruments.push_back(
            std::make_shared<QuantLib::SwapRateHelper>(
                rate, tenor, QuantLib::TARGET(), QuantLib::Annual, QuantLib::ModifiedFollowing,
                QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), euribor6m));
    }
    return std::make_shared<QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
        asOf, instruments, QuantLib::Actual365Fixed());
}

struct Reference {
    double npv, fair, points, baseLegPv, quoteLegPv;
    QuantLib::Date spotDate, maturity;
};

/// The documented identities, evaluated on the two reference curves.
Reference referencePrice(
    const QuantLib::Date& asOf,
    const QuantLib::Date& maturityGiven,
    double K,
    bool buyBase,
    bool reportInQuote,
    double eurRate = kEurRate,
    double usdRate = kUsdRate) {
    auto eur = referenceCurve(eurRate, asOf);
    auto usd = referenceCurve(usdRate, asOf);
    const QuantLib::Calendar cal = jointCalendar();
    Reference r;
    r.spotDate = cal.advance(cal.adjust(asOf), 2 * QuantLib::Days);
    r.maturity = cal.adjust(maturityGiven, QuantLib::ModifiedFollowing);
    const double sign = buyBase ? 1.0 : -1.0;
    const double dfB = eur->discount(r.maturity), dfQ = usd->discount(r.maturity);
    const double X = kSpot * usd->discount(r.spotDate) / eur->discount(r.spotDate);
    r.baseLegPv = sign * kNotional * dfB;
    r.quoteLegPv = -sign * kNotional * K * dfQ;
    r.npv = reportInQuote ? r.baseLegPv * X + r.quoteLegPv : r.baseLegPv + r.quoteLegPv / X;
    r.fair = X * dfB / dfQ;
    r.points = r.fair - kSpot;
    return r;
}

struct TradeFields {
    const char* base = "EUR";
    const char* quote = "USD";
    flatbuffers::Optional<double> notional = kNotional;
    flatbuffers::Optional<double> rate = 1.0900;
    const char* maturity = "2025-07-17";
    flatbuffers::Optional<quantra::enums::FxDirection> direction =
        quantra::enums::FxDirection_BuyBase;
    const char* spotId = "EURUSD_SPOT";
    const char* baseCurve = "EUR_DISC";
    const char* quoteCurve = "USD_DISC";
    const char* reporting = "USD";
};

} // namespace

class FxForwardParityTest : public QuantraComparisonTest {
protected:
    /// Builds and prices a one-forward request on the EUR 3% / USD 4.5%
    /// curves and the EUR/USD spot; `spotBase`/`spotQuote` let a test give
    /// the spot the other way round.
    const quantra::FxForwardResponse* price(
        flatbuffers::grpc::MessageBuilder& b,
        std::shared_ptr<flatbuffers::grpc::MessageBuilder>& rb,
        const TradeFields& t,
        const char* spotBase = "EUR",
        const char* spotQuote = "USD") {
        std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
            buildCurve(b, "EUR_DISC", kEurRate), buildCurve(b, "USD_DISC", kUsdRate)};
        auto curvesVec = b.CreateVector(curves);
        auto indices = buildIndicesVector(b);
        std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{quantra::CreateFxSpotSpecDirect(
            b, "EURUSD_SPOT", spotBase, spotQuote, kSpot, nullptr, 2, &kSettlementCalendars,
            quantra::enums::BusinessDayConvention_ModifiedFollowing)};
        auto fx = quantra::CreateFxMarketDataDirect(b, &spots);
        auto rates = quantra::CreateRatesMarketData(b, indices, 0, curvesVec, 0);
        auto asOf = b.CreateString("2025-01-15");
        auto pricing = quantra::CreatePricing(b, asOf, 0, 0, rates, 0, 0, 0, 0, 0, 0, fx);

        auto fwd = quantra::CreateFxForwardDirect(
            b, t.base, t.quote, t.notional, t.rate, t.maturity, t.direction);
        auto entry = quantra::CreatePriceFxForwardDirect(
            b, fwd, t.spotId, t.baseCurve, t.quoteCurve, t.reporting);
        std::vector<flatbuffers::Offset<quantra::PriceFxForward>> entries{entry};
        auto req = quantra::CreatePriceFxForwardRequestDirect(b, pricing, &entries);
        b.Finish(req);

        FxForwardEndpoint endpoint;
        rb = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto off = endpoint.request(
            rb, flatbuffers::GetRoot<quantra::PriceFxForwardRequest>(b.GetBufferPointer()));
        rb->Finish(off);
        auto response =
            flatbuffers::GetRoot<quantra::PriceFxForwardResponse>(rb->GetBufferPointer());
        EXPECT_EQ(response->fx_forwards()->size(), 1u);
        return response->fx_forwards()->Get(0);
    }

    std::string errorOf(const std::function<void()>& fn) {
        try {
            fn();
        } catch (const QuantraError& e) {
            return e.what();
        }
        return "";
    }

    // Amounts: 1e-12 of the notional (the bootstrap accuracy of the two
    // independent curve builds — a swap-pillar DF agrees to ~1e-14, which on
    // a 1e7 notional is 1e-7 of currency). Rates (DF ratios): 1e-12 relative.
    void expectMatches(const quantra::FxForwardResponse* r, const Reference& ref) {
        const double amountTol = 1e-12 * kNotional * std::max(1.0, kSpot);
        EXPECT_NEAR(r->npv(), ref.npv, amountTol);
        EXPECT_NEAR(r->fair_forward_rate(), ref.fair, 1e-12 * ref.fair);
        EXPECT_NEAR(r->forward_points(), ref.points, 1e-12);
        EXPECT_NEAR(r->base_leg_pv(), ref.baseLegPv, amountTol);
        EXPECT_NEAR(r->quote_leg_pv(), ref.quoteLegPv, amountTol);
        EXPECT_EQ(r->spot_date()->str(), DateToIso(ref.spotDate));
        EXPECT_EQ(r->maturity_date()->str(), DateToIso(ref.maturity));
        EXPECT_DOUBLE_EQ(r->fx_spot_used(), kSpot);
    }
};

// ---------------------------------------------------------------------------
// parity
// ---------------------------------------------------------------------------

TEST_F(FxForwardParityTest, BuyBase_ReportedInQuote_MatchesDiscountFactorAlgebra) {
    std::cout << "\n=== FX forward: buy EUR 6M, NPV in USD ===" << std::endl;
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    const auto* r = price(b, rb, TradeFields{});
    const auto ref = referencePrice(
        evaluationDate_, QuantLib::Date(17, QuantLib::July, 2025), 1.0900, true, true);
    std::cout << "  QuantLib npv " << std::setprecision(15) << ref.npv << " | Quantra " << r->npv()
              << " | fair " << ref.fair << " vs " << r->fair_forward_rate() << std::endl;
    expectMatches(r, ref);
    // The USD curve is steeper than the EUR one: the forward is above spot.
    EXPECT_GT(r->forward_points(), 0.0);
    EXPECT_EQ(r->spot_date()->str(), "2025-01-17");
    EXPECT_EQ(r->maturity_date()->str(), "2025-07-17");
}

TEST_F(FxForwardParityTest, SellBase_IsTheExactNegative) {
    flatbuffers::grpc::MessageBuilder b1, b2;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2;
    const auto* buy = price(b1, rb1, TradeFields{});
    TradeFields sell;
    sell.direction = quantra::enums::FxDirection_SellBase;
    const auto* sold = price(b2, rb2, sell);
    expectMatches(
        sold, referencePrice(
                  evaluationDate_, QuantLib::Date(17, QuantLib::July, 2025), 1.0900, false, true));
    EXPECT_NEAR(buy->npv() + sold->npv(), 0.0, 1e-9);
    EXPECT_NEAR(buy->base_leg_pv() + sold->base_leg_pv(), 0.0, 1e-9);
    EXPECT_NEAR(buy->quote_leg_pv() + sold->quote_leg_pv(), 0.0, 1e-9);
    EXPECT_DOUBLE_EQ(buy->fair_forward_rate(), sold->fair_forward_rate());
}

TEST_F(FxForwardParityTest, ReportedInBase_UsesTheAsOfExchangeRate) {
    flatbuffers::grpc::MessageBuilder b1, b2;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2;
    const auto* inUsd = price(b1, rb1, TradeFields{});
    TradeFields t;
    t.reporting = "EUR";
    const auto* inEur = price(b2, rb2, t);
    expectMatches(
        inEur, referencePrice(
                   evaluationDate_, QuantLib::Date(17, QuantLib::July, 2025), 1.0900, true, false));
    // npv_EUR * X == npv_USD with X = S * DF_usd(spot) / DF_eur(spot).
    auto eur = referenceCurve(kEurRate, evaluationDate_);
    auto usd = referenceCurve(kUsdRate, evaluationDate_);
    const QuantLib::Date spotDate = DateToQL(inUsd->spot_date()->str());
    const double X = kSpot * usd->discount(spotDate) / eur->discount(spotDate);
    EXPECT_NEAR(inEur->npv() * X, inUsd->npv(), 1e-12 * std::abs(inUsd->npv()));
    EXPECT_DOUBLE_EQ(inEur->base_leg_pv(), inUsd->base_leg_pv());
    EXPECT_DOUBLE_EQ(inEur->quote_leg_pv(), inUsd->quote_leg_pv());
}

TEST_F(FxForwardParityTest, RepricedAtFairForwardHasZeroNpv) {
    flatbuffers::grpc::MessageBuilder b1, b2, b3;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2, rb3;
    const double fair = price(b1, rb1, TradeFields{})->fair_forward_rate();
    TradeFields atFair;
    atFair.rate = fair;
    EXPECT_NEAR(price(b2, rb2, atFair)->npv(), 0.0, 1e-9 * kNotional);
    atFair.reporting = "EUR";
    atFair.direction = quantra::enums::FxDirection_SellBase;
    EXPECT_NEAR(price(b3, rb3, atFair)->npv(), 0.0, 1e-9 * kNotional);
}

TEST_F(FxForwardParityTest, SameCurveBothSides_FairEqualsSpot) {
    // With one curve discounting both legs the as-of rate is the spot and the
    // fair forward equals the spot: points are zero and npv = N * DF * (S - K).
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    TradeFields t;
    t.quoteCurve = "EUR_DISC";
    const auto* r = price(b, rb, t);
    EXPECT_NEAR(r->fair_forward_rate(), kSpot, 1e-14);
    EXPECT_NEAR(r->forward_points(), 0.0, 1e-14);
    const double df = discountHandle_->discount(DateToQL(r->maturity_date()->str()));
    EXPECT_NEAR(r->npv(), kNotional * df * (kSpot - 1.0900), 1e-6);
}

TEST_F(FxForwardParityTest, MaturityAdjustedOnTheJointCalendar) {
    // Saturday 2026-01-17 -> Monday 2026-01-19 is a US holiday -> 2026-01-20.
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    TradeFields t;
    t.maturity = "2026-01-17";
    const auto* r = price(b, rb, t);
    EXPECT_EQ(r->maturity_date()->str(), "2026-01-20");
    expectMatches(
        r, referencePrice(
               evaluationDate_, QuantLib::Date(17, QuantLib::January, 2026), 1.0900, true, true));
}

// ---------------------------------------------------------------------------
// contract
// ---------------------------------------------------------------------------

TEST_F(FxForwardParityTest, InvertedPairIsRejectedNotInverted) {
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    TradeFields t;
    t.base = "USD";
    t.quote = "EUR";
    t.reporting = "EUR";
    const std::string msg = errorOf([&] { price(b, rb, t); });
    EXPECT_NE(msg.find("PriceFxForward.fx_spot"), std::string::npos) << msg;
    EXPECT_NE(msg.find("USD/EUR"), std::string::npos) << msg;
    // The same trade against a spot quoted USD/EUR is fine (and is a 400 the
    // other way round): the spot's order is the forward's order.
    flatbuffers::grpc::MessageBuilder b2;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb2;
    EXPECT_EQ(errorOf([&] { price(b2, rb2, t, "USD", "EUR"); }), "");
}

TEST_F(FxForwardParityTest, MapperValidationsName_TheField) {
    auto check = [&](TradeFields t, const char* expected) {
        flatbuffers::grpc::MessageBuilder b;
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
        const std::string msg = errorOf([&] { price(b, rb, t); });
        EXPECT_NE(msg.find(expected), std::string::npos)
            << "expected '" << expected << "' in '" << msg << "'";
    };
    TradeFields t;
    t.notional = flatbuffers::Optional<double>();
    check(t, "FxForward.base_notional is required");
    t = TradeFields{};
    t.notional = 0.0;
    check(t, "FxForward.base_notional must be positive");
    t = TradeFields{};
    t.rate = flatbuffers::Optional<double>();
    check(t, "FxForward.forward_rate is required");
    t = TradeFields{};
    t.rate = -1.0;
    check(t, "FxForward.forward_rate must be positive");
    t = TradeFields{};
    t.direction = flatbuffers::Optional<quantra::enums::FxDirection>();
    check(t, "FxForward.direction is required");
    t = TradeFields{};
    t.base = "EURUSD";
    check(t, "FxForward.base_currency must be a three-letter uppercase ISO currency code");
    t = TradeFields{};
    t.quote = "EUR";
    check(t, "FxForward.base_currency and FxForward.quote_currency must differ");
    t = TradeFields{};
    t.reporting = "GBP";
    check(t, "PriceFxForward.reporting_currency 'GBP' must be EUR or USD");
    t = TradeFields{};
    t.maturity = "2025/07/17";
    check(t, "Invalid date");
    t = TradeFields{};
    t.maturity = "2025-01-17"; // the spot date
    check(
        t, "FxForward.maturity_date 2025-01-17 (adjusted 2025-01-17) must fall after the spot "
           "date 2025-01-17");
}

TEST_F(FxForwardParityTest, UnknownReferencesAre404) {
    auto notFound = [&](TradeFields t, const char* expected) {
        flatbuffers::grpc::MessageBuilder b;
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
        try {
            price(b, rb, t);
            FAIL() << "expected QuantraNotFound for " << expected;
        } catch (const QuantraNotFound& e) {
            EXPECT_NE(std::string(e.what()).find(expected), std::string::npos) << e.what();
        }
    };
    TradeFields t;
    t.spotId = "NOPE";
    notFound(t, "FX spot not found: NOPE");
    t = TradeFields{};
    t.baseCurve = "NOPE";
    notFound(t, "Base discounting curve not found: NOPE");
    t = TradeFields{};
    t.quoteCurve = "NOPE";
    notFound(t, "Quote discounting curve not found: NOPE");
}

TEST_F(FxForwardParityTest, EmptyForwardListIs400) {
    flatbuffers::grpc::MessageBuilder b;
    auto asOf = b.CreateString("2025-01-15");
    auto pricing = quantra::CreatePricing(b, asOf);
    std::vector<flatbuffers::Offset<quantra::PriceFxForward>> none;
    b.Finish(quantra::CreatePriceFxForwardRequestDirect(b, pricing, &none));
    FxForwardEndpoint endpoint;
    auto rb = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    const std::string msg = errorOf([&] {
        endpoint.request(
            rb, flatbuffers::GetRoot<quantra::PriceFxForwardRequest>(b.GetBufferPointer()));
    });
    EXPECT_NE(msg.find("PriceFxForwardRequest.fx_forwards"), std::string::npos) << msg;
}

}} // namespace quantra::testing
