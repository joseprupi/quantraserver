// Cross-currency swap parity and contract tests.
//
// Parity: the endpoint's NPVs, fair rate / fair spread and as-of rate must
// equal the cash-flow algebra the product documents
// (cross_currency_swap_evaluator.h), evaluated here on QuantLib legs built
// independently of the engine (FixedRateLeg / IborLeg / OvernightLeg +
// SimpleCashFlow exchanges, CashFlows::npv / bps) on curves bootstrapped from
// the fixture's helper set at a flat rate per currency: amounts to 1e-9 of
// the notional (the two bootstraps agree to ~1e-14 on the discount factors;
// 25 flows on a 1e7 notional stay well inside 1e-2 currency units), rates to
// 1e-12 relative. Side, reporting currency, the reprice-at-fair round trips,
// the exchange conventions and the flows are pinned.
//
// Contract: the mapper's validations are 400s naming the field; the leg
// currency must be its index's; unknown references are 404s.
#include "cross_currency_swap_handler.h"
#include "parity_fixture.h"

#include "cross_currency_swap_response_generated.h"
#include "price_cross_currency_swap_request_generated.h"

#include <ql/cashflows/cashflows.hpp>
#include <ql/cashflows/couponpricer.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/cashflows/overnightindexedcoupon.hpp>
#include <ql/cashflows/simplecashflow.hpp>
#include <ql/currencies/america.hpp>
#include <ql/currencies/europe.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/time/calendars/jointcalendar.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedstates.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/time/daycounters/thirty360.hpp>

#include <cmath>
#include <functional>
#include <optional>

namespace quantra { namespace testing {

namespace {

constexpr double kSpot = 1.0850;
constexpr double kEurNotional = 10000000.0;
constexpr double kUsdNotional = 10850000.0;
constexpr double kEurRate = 0.03; // = the fixture's flatRate_ (discountHandle_)
constexpr double kUsdRate = 0.045;
constexpr double kFixedRate = 0.0255;
constexpr double kBasisPoint = 1.0e-4;

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

QuantLib::Schedule schedule(QuantLib::Frequency freq) {
    return QuantLib::Schedule(
        QuantLib::Date(17, QuantLib::January, 2025), QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(freq), QuantLib::TARGET(), QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
}

enum class LegKind { Fixed, Overnight, Ibor };

struct LegFields {
    const char* currency;
    LegKind kind;
    double notional;
    double level; // fixed rate or spread
    bool initialExchange = true;
    bool finalExchange = true;
    const char* discountingCurve;
    const char* forwardingCurve = nullptr; // floating only
    const char* indexId = nullptr;         // floating only
};

struct TradeFields {
    LegFields leg1{"EUR", LegKind::Fixed, kEurNotional, kFixedRate, true, true, "EUR_DISC"};
    LegFields leg2{"USD", LegKind::Overnight, kUsdNotional, 0.0,       true,
                   true,  "USD_DISC",         "USD_DISC",   "USD_SOFR"};
    flatbuffers::Optional<quantra::enums::SwapType> swapType = quantra::enums::SwapType_Payer;
    const char* spotId = "EURUSD_SPOT";
    const char* reporting = "USD";
};

/// Reference value of one leg in its own frame on an independent curve.
struct RefLeg {
    double pvOwn = 0.0;
    double annuity = 0.0;
    double dfStart = 0.0;
};

RefLeg referenceLeg(const LegFields& leg, const QuantLib::Date& asOf) {
    const double rate = std::string(leg.currency) == "EUR" ? kEurRate : kUsdRate;
    auto curve = referenceCurve(rate, asOf);
    QuantLib::Handle<QuantLib::YieldTermStructure> handle(curve);
    QuantLib::Leg coupons;
    QuantLib::Schedule sch = schedule(
        leg.kind == LegKind::Fixed       ? QuantLib::Annual
        : leg.kind == LegKind::Overnight ? QuantLib::Quarterly
                                         : QuantLib::Semiannual);
    if (leg.kind == LegKind::Fixed) {
        coupons =
            QuantLib::FixedRateLeg(sch)
                .withNotionals(leg.notional)
                .withCouponRates(leg.level, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis))
                .withPaymentAdjustment(QuantLib::ModifiedFollowing);
    } else if (leg.kind == LegKind::Overnight) {
        auto sofr = std::make_shared<QuantLib::OvernightIndex>(
            "SOFR", 0, QuantLib::USDCurrency(),
            QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond), QuantLib::Actual360(),
            handle);
        coupons = QuantLib::OvernightLeg(sch, sofr)
                      .withNotionals(leg.notional)
                      .withPaymentDayCounter(QuantLib::Actual360())
                      .withPaymentAdjustment(QuantLib::ModifiedFollowing)
                      .withSpreads(leg.level);
    } else {
        auto euribor6m = std::make_shared<QuantLib::IborIndex>(
            "Euribor", 6 * QuantLib::Months, 2, QuantLib::EURCurrency(), QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, false, QuantLib::Actual360(), handle);
        coupons = QuantLib::IborLeg(sch, euribor6m)
                      .withNotionals(leg.notional)
                      .withPaymentDayCounter(QuantLib::Actual360())
                      .withPaymentAdjustment(QuantLib::ModifiedFollowing)
                      .withSpreads(leg.level)
                      .withFixingDays(2);
        auto vol = std::make_shared<QuantLib::ConstantOptionletVolatility>(
            0, QuantLib::TARGET(), QuantLib::ModifiedFollowing, 0.0, QuantLib::Actual365Fixed());
        auto pricer = std::make_shared<QuantLib::BlackIborCouponPricer>();
        pricer->setCapletVolatility(QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(vol));
        QuantLib::setCouponPricer(coupons, pricer);
    }
    QuantLib::Leg full = coupons;
    if (leg.initialExchange) {
        full.push_back(
            std::make_shared<QuantLib::SimpleCashFlow>(-leg.notional, sch.dates().front()));
    }
    if (leg.finalExchange) {
        full.push_back(
            std::make_shared<QuantLib::SimpleCashFlow>(leg.notional, coupons.back()->date()));
    }
    RefLeg r;
    r.pvOwn = QuantLib::CashFlows::npv(full, *curve, false, asOf, asOf);
    r.annuity = QuantLib::CashFlows::bps(coupons, *curve, false, asOf, asOf) / kBasisPoint;
    r.dfStart = curve->discount(sch.dates().front());
    return r;
}

struct Reference {
    double npv, leg1Npv, leg2Npv, leg1Rep, leg2Rep, X;
    std::optional<double> fairRate, fairSpread;
};

/// The documented identities, evaluated on the two reference curves.
Reference referencePrice(const TradeFields& t, const QuantLib::Date& asOf) {
    auto eur = referenceCurve(kEurRate, asOf);
    auto usd = referenceCurve(kUsdRate, asOf);
    const QuantLib::Calendar cal = jointCalendar();
    const QuantLib::Date spotDate = cal.advance(cal.adjust(asOf), 2 * QuantLib::Days);
    Reference r;
    r.X = kSpot * usd->discount(spotDate) / eur->discount(spotDate);
    auto convert = [&](double amount, const std::string& from, const std::string& to) {
        if (from == to) return amount;
        return from == "EUR" ? amount * r.X : amount / r.X;
    };
    const RefLeg l1 = referenceLeg(t.leg1, asOf), l2 = referenceLeg(t.leg2, asOf);
    const double s1 = t.swapType.value() == quantra::enums::SwapType_Payer ? -1.0 : 1.0;
    const double s2 = -s1;
    r.leg1Npv = s1 * l1.pvOwn;
    r.leg2Npv = s2 * l2.pvOwn;
    r.leg1Rep = convert(r.leg1Npv, t.leg1.currency, t.reporting);
    r.leg2Rep = convert(r.leg2Npv, t.leg2.currency, t.reporting);
    r.npv = r.leg1Rep + r.leg2Rep;
    auto npvIn = [&](const std::string& ccy) {
        return convert(r.leg1Npv, t.leg1.currency, ccy) + convert(r.leg2Npv, t.leg2.currency, ccy);
    };
    const bool fixed1 = t.leg1.kind == LegKind::Fixed, fixed2 = t.leg2.kind == LegKind::Fixed;
    if (fixed1 != fixed2) {
        const LegFields& f = fixed1 ? t.leg1 : t.leg2;
        r.fairRate =
            f.level - npvIn(f.currency) / ((fixed1 ? s1 : s2) * (fixed1 ? l1 : l2).annuity);
    } else if (!fixed1) {
        r.fairSpread = t.leg2.level - npvIn(t.leg2.currency) / (s2 * l2.annuity);
    }
    return r;
}

} // namespace

class CrossCurrencySwapParityTest : public QuantraComparisonTest {
protected:
    flatbuffers::Offset<quantra::Schedule> fbSchedule(
        flatbuffers::grpc::MessageBuilder& b, quantra::enums::Frequency freq) {
        auto eff = b.CreateString("2025-01-17");
        auto term = b.CreateString("2030-01-17");
        quantra::ScheduleBuilder sb(b);
        sb.add_effective_date(eff);
        sb.add_termination_date(term);
        sb.add_calendar(quantra::enums::Calendar_TARGET);
        sb.add_frequency(freq);
        sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        sb.add_end_of_month(false);
        return sb.Finish();
    }

    flatbuffers::Offset<quantra::XccyLeg> fbLeg(
        flatbuffers::grpc::MessageBuilder& b, const LegFields& leg) {
        flatbuffers::Offset<quantra::SwapFixedLeg> fixed = 0;
        flatbuffers::Offset<quantra::SwapFloatingLeg> floating = 0;
        if (leg.kind == LegKind::Fixed) {
            auto sch = fbSchedule(b, quantra::enums::Frequency_Annual);
            quantra::SwapFixedLegBuilder fl(b);
            fl.add_notional(leg.notional);
            fl.add_schedule(sch);
            fl.add_rate(leg.level);
            fl.add_day_counter(quantra::enums::DayCounter_Thirty360);
            fl.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            fixed = fl.Finish();
        } else {
            auto sch = fbSchedule(
                b, leg.kind == LegKind::Overnight ? quantra::enums::Frequency_Quarterly
                                                  : quantra::enums::Frequency_Semiannual);
            auto idx = buildIndexRef(b, leg.indexId);
            quantra::SwapFloatingLegBuilder fl(b);
            fl.add_notional(leg.notional);
            fl.add_schedule(sch);
            fl.add_index(idx);
            fl.add_spread(leg.level);
            fl.add_day_counter(quantra::enums::DayCounter_Actual360);
            fl.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
            floating = fl.Finish();
        }
        auto ccy = b.CreateString(leg.currency);
        auto disc = b.CreateString(leg.discountingCurve);
        flatbuffers::Offset<flatbuffers::String> fwd = 0;
        if (leg.forwardingCurve) fwd = b.CreateString(leg.forwardingCurve);
        quantra::XccyLegBuilder xl(b);
        xl.add_currency(ccy);
        if (leg.kind == LegKind::Fixed) {
            xl.add_fixed_leg(fixed);
        } else {
            xl.add_floating_leg(floating);
        }
        xl.add_initial_exchange(leg.initialExchange);
        xl.add_final_exchange(leg.finalExchange);
        xl.add_discounting_curve(disc);
        if (leg.forwardingCurve) xl.add_forwarding_curve(fwd);
        return xl.Finish();
    }

    /// Builds and prices a one-swap request on the EUR 3% / USD 4.5% curves
    /// (EUR_6M and USD_SOFR indices) and the EUR/USD spot.
    const quantra::CrossCurrencySwapResponse* price(
        flatbuffers::grpc::MessageBuilder& b,
        std::shared_ptr<flatbuffers::grpc::MessageBuilder>& rb,
        const TradeFields& t,
        bool includeFlows = false,
        const char* spotBase = "EUR",
        const char* spotQuote = "USD",
        double spotRate = kSpot) {
        std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
            buildCurve(b, "EUR_DISC", kEurRate), buildCurve(b, "USD_DISC", kUsdRate)};
        auto curvesVec = b.CreateVector(curves);
        auto indices = buildIndicesVectorWithSofr(b);
        std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{quantra::CreateFxSpotSpecDirect(
            b, "EURUSD_SPOT", spotBase, spotQuote, spotRate, nullptr, 2, &kSettlementCalendars,
            quantra::enums::BusinessDayConvention_ModifiedFollowing)};
        auto fx = quantra::CreateFxMarketDataDirect(b, &spots);
        auto rates = quantra::CreateRatesMarketData(b, indices, 0, curvesVec, 0);
        auto asOf = b.CreateString("2025-01-15");
        auto pricing = quantra::CreatePricing(b, asOf, 0, 0, rates, 0, 0, 0, 0, 0, 0, fx);

        auto leg1 = fbLeg(b, t.leg1);
        auto leg2 = fbLeg(b, t.leg2);
        quantra::CrossCurrencySwapBuilder sb(b);
        if (t.swapType.has_value()) sb.add_swap_type(t.swapType.value());
        sb.add_leg1(leg1);
        sb.add_leg2(leg2);
        auto swap = sb.Finish();
        auto entry = quantra::CreatePriceCrossCurrencySwapDirect(b, swap, t.spotId, t.reporting);
        std::vector<flatbuffers::Offset<quantra::PriceCrossCurrencySwap>> entries{entry};
        auto req =
            quantra::CreatePriceCrossCurrencySwapRequestDirect(b, pricing, &entries, includeFlows);
        b.Finish(req);

        CrossCurrencySwapEndpoint endpoint;
        rb = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto off = endpoint.request(
            rb, flatbuffers::GetRoot<quantra::PriceCrossCurrencySwapRequest>(b.GetBufferPointer()));
        rb->Finish(off);
        auto response =
            flatbuffers::GetRoot<quantra::PriceCrossCurrencySwapResponse>(rb->GetBufferPointer());
        EXPECT_EQ(response->swaps()->size(), 1u);
        return response->swaps()->Get(0);
    }

    std::string errorOf(const std::function<void()>& fn) {
        try {
            fn();
        } catch (const QuantraError& e) {
            return e.what();
        }
        return "";
    }

    void expectMatches(const quantra::CrossCurrencySwapResponse* r, const Reference& ref) {
        const double amountTol = 1e-9 * kUsdNotional;
        EXPECT_NEAR(r->npv(), ref.npv, amountTol);
        EXPECT_NEAR(r->leg1_npv(), ref.leg1Npv, amountTol);
        EXPECT_NEAR(r->leg2_npv(), ref.leg2Npv, amountTol);
        EXPECT_NEAR(r->leg1_npv_reporting(), ref.leg1Rep, amountTol);
        EXPECT_NEAR(r->leg2_npv_reporting(), ref.leg2Rep, amountTol);
        EXPECT_NEAR(r->fx_as_of_rate(), ref.X, 1e-12 * ref.X);
        EXPECT_DOUBLE_EQ(r->fx_spot_used(), kSpot);
        EXPECT_EQ(r->spot_date()->str(), "2025-01-17");
        EXPECT_EQ(r->fair_rate().has_value(), ref.fairRate.has_value());
        EXPECT_EQ(r->fair_spread().has_value(), ref.fairSpread.has_value());
        if (ref.fairRate) EXPECT_NEAR(r->fair_rate().value(), *ref.fairRate, 1e-12);
        if (ref.fairSpread) EXPECT_NEAR(r->fair_spread().value(), *ref.fairSpread, 1e-12);
    }
};

// ---------------------------------------------------------------------------
// parity
// ---------------------------------------------------------------------------

TEST_F(CrossCurrencySwapParityTest, FixedVsOvernight_MatchesCashFlowAlgebra) {
    std::cout << "\n=== Cross-currency swap: pay EUR 2.55% / receive USD SOFR, NPV in USD ==="
              << std::endl;
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    const TradeFields t;
    const auto* r = price(b, rb, t);
    const auto ref = referencePrice(t, evaluationDate_);
    std::cout << "  QuantLib npv " << std::setprecision(15) << ref.npv << " | Quantra " << r->npv()
              << " | fair rate " << *ref.fairRate << " vs " << r->fair_rate().value() << std::endl;
    expectMatches(r, ref);
    // Paying 2.55% fixed on a flat 3% curve (with both exchanges: a bond
    // bought below par) is in the payer's favour; the received SOFR leg with
    // its exchanges is a par floater on its own curve, NPV ~ 0.
    EXPECT_GT(r->leg1_npv(), 0.0);
    EXPECT_NEAR(r->leg2_npv(), 0.0, 1e-3 * kUsdNotional);
    EXPECT_GT(r->npv(), 0.0);
    EXPECT_GT(r->fair_rate().value(), kFixedRate);
    EXPECT_DOUBLE_EQ(r->leg2_npv_reporting(), r->leg2_npv());
    EXPECT_FALSE(r->leg1_flows());
}

TEST_F(CrossCurrencySwapParityTest, FloatVsFloat_MatchesCashFlowAlgebra) {
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    TradeFields t;
    t.leg1 = LegFields{"EUR", LegKind::Ibor, kEurNotional, -0.0015, true,
                       true,  "EUR_DISC",    "EUR_DISC",   "EUR_6M"};
    t.leg2 = LegFields{"USD", LegKind::Overnight, kUsdNotional, 0.0,       true,
                       true,  "USD_DISC",         "USD_DISC",   "USD_SOFR"};
    const auto* r = price(b, rb, t);
    expectMatches(r, referencePrice(t, evaluationDate_));
    EXPECT_TRUE(r->fair_spread().has_value());
    EXPECT_FALSE(r->fair_rate().has_value());
}

TEST_F(CrossCurrencySwapParityTest, ReceiverIsTheExactNegative) {
    flatbuffers::grpc::MessageBuilder b1, b2;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2;
    const auto* payer = price(b1, rb1, TradeFields{});
    TradeFields t;
    t.swapType = quantra::enums::SwapType_Receiver;
    const auto* receiver = price(b2, rb2, t);
    expectMatches(receiver, referencePrice(t, evaluationDate_));
    EXPECT_DOUBLE_EQ(payer->npv(), -receiver->npv());
    EXPECT_DOUBLE_EQ(payer->leg1_npv(), -receiver->leg1_npv());
    EXPECT_DOUBLE_EQ(payer->leg2_npv(), -receiver->leg2_npv());
    EXPECT_DOUBLE_EQ(payer->fair_rate().value(), receiver->fair_rate().value());
}

TEST_F(CrossCurrencySwapParityTest, ReportedInBase_UsesTheAsOfExchangeRate) {
    flatbuffers::grpc::MessageBuilder b1, b2;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2;
    const auto* inUsd = price(b1, rb1, TradeFields{});
    TradeFields t;
    t.reporting = "EUR";
    const auto* inEur = price(b2, rb2, t);
    expectMatches(inEur, referencePrice(t, evaluationDate_));
    const double X = inUsd->fx_as_of_rate();
    EXPECT_DOUBLE_EQ(inEur->fx_as_of_rate(), X);
    EXPECT_NEAR(inEur->npv() * X, inUsd->npv(), 1e-12 * std::abs(inUsd->npv()));
    EXPECT_DOUBLE_EQ(inEur->leg1_npv(), inUsd->leg1_npv());
    EXPECT_DOUBLE_EQ(inEur->leg2_npv(), inUsd->leg2_npv());
    EXPECT_DOUBLE_EQ(inEur->leg1_npv_reporting(), inEur->leg1_npv());
    EXPECT_DOUBLE_EQ(inUsd->leg1_npv_reporting(), inUsd->leg1_npv() * X);
    EXPECT_DOUBLE_EQ(inEur->leg2_npv_reporting(), inEur->leg2_npv() / X);
}

TEST_F(CrossCurrencySwapParityTest, RepricedAtFairRateHasZeroNpv) {
    flatbuffers::grpc::MessageBuilder b1, b2, b3;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2, rb3;
    const double fair = price(b1, rb1, TradeFields{})->fair_rate().value();
    TradeFields atFair;
    atFair.leg1.level = fair;
    EXPECT_NEAR(price(b2, rb2, atFair)->npv(), 0.0, 1e-9 * kEurNotional);
    atFair.reporting = "EUR";
    atFair.swapType = quantra::enums::SwapType_Receiver;
    const auto* r = price(b3, rb3, atFair);
    EXPECT_NEAR(r->npv(), 0.0, 1e-9 * kEurNotional);
    EXPECT_NEAR(r->fair_rate().value(), fair, 1e-12);
}

TEST_F(CrossCurrencySwapParityTest, RepricedAtFairSpreadHasZeroNpv) {
    flatbuffers::grpc::MessageBuilder b1, b2;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2;
    TradeFields t;
    t.leg1 = LegFields{"USD", LegKind::Overnight, kUsdNotional, 0.0,       true,
                       true,  "USD_DISC",         "USD_DISC",   "USD_SOFR"};
    t.leg2 = LegFields{"EUR", LegKind::Ibor, kEurNotional, -0.0015, true,
                       true,  "EUR_DISC",    "EUR_DISC",   "EUR_6M"};
    const double fair = price(b1, rb1, t)->fair_spread().value();
    t.leg2.level = fair;
    const auto* r = price(b2, rb2, t);
    EXPECT_NEAR(r->npv(), 0.0, 1e-9 * kUsdNotional);
    EXPECT_NEAR(r->fair_spread().value(), fair, 1e-12);
}

TEST_F(CrossCurrencySwapParityTest, ExchangeFlagsAddExactlyThePvOfTheNotionalFlows) {
    // Payer: leg1 paid (s1 = -1), leg2 received (s2 = +1). Removing the
    // initial exchange adds +N*DF(start) to a leg's own-frame PV.
    flatbuffers::grpc::MessageBuilder b1, b2, b3;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2, rb3;
    const auto* both = price(b1, rb1, TradeFields{});
    TradeFields noInitial;
    noInitial.leg1.initialExchange = false;
    noInitial.leg2.initialExchange = false;
    const auto* finalOnly = price(b2, rb2, noInitial);
    const RefLeg l1 = referenceLeg(TradeFields{}.leg1, evaluationDate_);
    const RefLeg l2 = referenceLeg(TradeFields{}.leg2, evaluationDate_);
    EXPECT_NEAR(
        finalOnly->leg1_npv(), both->leg1_npv() - kEurNotional * l1.dfStart, 1e-9 * kEurNotional);
    EXPECT_NEAR(
        finalOnly->leg2_npv(), both->leg2_npv() + kUsdNotional * l2.dfStart, 1e-9 * kUsdNotional);
    expectMatches(finalOnly, referencePrice(noInitial, evaluationDate_));
    TradeFields none = noInitial;
    none.leg1.finalExchange = false;
    none.leg2.finalExchange = false;
    expectMatches(price(b3, rb3, none), referencePrice(none, evaluationDate_));
}

TEST_F(CrossCurrencySwapParityTest, FlowsListCouponsThenExchangesWithKind) {
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    const auto* r = price(b, rb, TradeFields{}, true);
    ASSERT_TRUE(r->leg1_flows() && r->leg2_flows());
    ASSERT_EQ(r->leg1_flows()->size(), 5u + 2u);  // annual fixed + 2 exchanges
    ASSERT_EQ(r->leg2_flows()->size(), 20u + 2u); // quarterly SOFR + 2 exchanges
    auto check = [&](const flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegFlow>>* flows,
                     double notional, double legNpv, double sign, bool floating) {
        double pv = 0.0;
        const unsigned n = flows->size();
        for (unsigned i = 0; i < n; ++i) {
            const auto* f = flows->Get(i);
            ASSERT_TRUE(f->kind().has_value());
            pv += f->present_value();
            if (i < n - 2) {
                EXPECT_EQ(f->kind().value(), quantra::enums::SwapFlowKind_Coupon);
                EXPECT_GT(f->amount(), 0.0);
                EXPECT_TRUE(f->accrual_start_date());
                EXPECT_EQ(f->fixing_date() != nullptr, floating);
            } else {
                EXPECT_EQ(f->kind().value(), quantra::enums::SwapFlowKind_NotionalExchange);
                EXPECT_FALSE(f->accrual_start_date());
                EXPECT_FALSE(f->fixing_date());
            }
        }
        EXPECT_DOUBLE_EQ(flows->Get(n - 2)->amount(), -notional);
        EXPECT_EQ(flows->Get(n - 2)->payment_date()->str(), "2025-01-17");
        EXPECT_DOUBLE_EQ(flows->Get(n - 1)->amount(), notional);
        EXPECT_EQ(
            flows->Get(n - 1)->payment_date()->str(), flows->Get(n - 3)->payment_date()->str());
        EXPECT_NEAR(pv, legNpv / sign, 1e-6);
    };
    check(r->leg1_flows(), kEurNotional, r->leg1_npv(), -1.0, false);
    check(r->leg2_flows(), kUsdNotional, r->leg2_npv(), +1.0, true);
}

TEST_F(CrossCurrencySwapParityTest, FixedFixedHasNoFairQuantity) {
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    TradeFields t;
    t.leg2 = LegFields{"USD", LegKind::Fixed, kUsdNotional, 0.041, true, true, "USD_DISC"};
    const auto* r = price(b, rb, t);
    expectMatches(r, referencePrice(t, evaluationDate_));
    EXPECT_FALSE(r->fair_rate().has_value());
    EXPECT_FALSE(r->fair_spread().has_value());
}

TEST_F(CrossCurrencySwapParityTest, SpotQuotedTheOtherWayRoundIsAccepted) {
    // USD/EUR at 1/S with the same legs: X inverts, every NPV is unchanged.
    flatbuffers::grpc::MessageBuilder b1, b2;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb1, rb2;
    const auto* eurusd = price(b1, rb1, TradeFields{});
    const auto* usdeur = price(b2, rb2, TradeFields{}, false, "USD", "EUR", 1.0 / kSpot);
    EXPECT_NEAR(usdeur->fx_as_of_rate() * eurusd->fx_as_of_rate(), 1.0, 1e-12);
    EXPECT_NEAR(usdeur->npv(), eurusd->npv(), 1e-6);
    EXPECT_DOUBLE_EQ(usdeur->leg1_npv(), eurusd->leg1_npv());
    EXPECT_DOUBLE_EQ(usdeur->leg2_npv(), eurusd->leg2_npv());
}

// ---------------------------------------------------------------------------
// contract
// ---------------------------------------------------------------------------

TEST_F(CrossCurrencySwapParityTest, MapperValidationsName_TheField) {
    auto check = [&](TradeFields t, const char* expected) {
        flatbuffers::grpc::MessageBuilder b;
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
        const std::string msg = errorOf([&] { price(b, rb, t); });
        EXPECT_NE(msg.find(expected), std::string::npos)
            << "expected '" << expected << "' in '" << msg << "'";
    };
    TradeFields t;
    t.swapType = flatbuffers::Optional<quantra::enums::SwapType>();
    check(t, "CrossCurrencySwap.swap_type is required");
    t = TradeFields{};
    t.leg1.notional = 0.0;
    check(t, "CrossCurrencySwap.leg1.fixed_leg.notional must be positive");
    t = TradeFields{};
    t.leg2.notional = -1.0;
    check(t, "CrossCurrencySwap.leg2.floating_leg.notional must be positive");
    t = TradeFields{};
    t.leg1.forwardingCurve = "EUR_DISC";
    check(t, "CrossCurrencySwap.leg1.forwarding_curve must be absent with a fixed_leg");
    t = TradeFields{};
    t.leg2.forwardingCurve = nullptr;
    check(t, "CrossCurrencySwap.leg2.forwarding_curve is required with a floating_leg");
    t = TradeFields{};
    t.leg1.currency = "EURUSD";
    check(t, "CrossCurrencySwap.leg1.currency must be a three-letter uppercase ISO currency code");
    t = TradeFields{};
    t.leg2.currency = "EUR";
    t.leg2.kind = LegKind::Fixed;
    t.leg2.forwardingCurve = nullptr;
    check(t, "CrossCurrencySwap.leg1.currency and CrossCurrencySwap.leg2.currency must differ");
    t = TradeFields{};
    t.reporting = "GBP";
    check(t, "PriceCrossCurrencySwap.reporting_currency 'GBP' must be EUR or USD");
    // Evaluator-side: the leg currency must be its index's, reported before
    // the spot-pair check (per-leg checks come first).
    t = TradeFields{};
    t.leg2.currency = "GBP";
    t.reporting = "EUR";
    check(
        t,
        "XccyLeg.currency 'GBP' of leg2 does not match the currency USD of its index 'USD_SOFR'");
}

TEST_F(CrossCurrencySwapParityTest, SpotMustQuoteTheLegsPair) {
    flatbuffers::grpc::MessageBuilder b;
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> rb;
    const std::string msg = errorOf([&] { price(b, rb, TradeFields{}, false, "EUR", "GBP"); });
    EXPECT_NE(msg.find("PriceCrossCurrencySwap.fx_spot"), std::string::npos) << msg;
    EXPECT_NE(msg.find("EUR/GBP"), std::string::npos) << msg;
}

TEST_F(CrossCurrencySwapParityTest, UnknownReferencesAre404) {
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
    t.leg1.discountingCurve = "NOPE";
    notFound(t, "leg1 discounting curve not found: NOPE");
    t = TradeFields{};
    t.leg2.forwardingCurve = "NOPE";
    notFound(t, "leg2 forwarding curve not found: NOPE");
    t = TradeFields{};
    t.leg2.indexId = "NOPE";
    notFound(t, "Index not found: NOPE");
}

TEST_F(CrossCurrencySwapParityTest, EmptySwapListIs400) {
    flatbuffers::grpc::MessageBuilder b;
    auto asOf = b.CreateString("2025-01-15");
    auto pricing = quantra::CreatePricing(b, asOf);
    std::vector<flatbuffers::Offset<quantra::PriceCrossCurrencySwap>> none;
    b.Finish(quantra::CreatePriceCrossCurrencySwapRequestDirect(b, pricing, &none));
    CrossCurrencySwapEndpoint endpoint;
    auto rb = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    const std::string msg = errorOf([&] {
        endpoint.request(
            rb, flatbuffers::GetRoot<quantra::PriceCrossCurrencySwapRequest>(b.GetBufferPointer()));
    });
    EXPECT_NE(msg.find("PriceCrossCurrencySwapRequest.swaps"), std::string::npos) << msg;
}

}} // namespace quantra::testing
