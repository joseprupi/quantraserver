// FX market data (FxSpotSpec) + FX swap helper (FxSwapHelper) parity and
// contract tests.
//
// Parity: a USD-collateralised EUR discount curve bootstrapped from FX swap
// points through the curve-sampling endpoint must reproduce, discount factor
// by discount factor, a QuantLib PiecewiseYieldCurve built directly from
// FxSwapRateHelper with the same spot, points, lag, joint calendar and
// collateral curve. The FX curve is listed BEFORE its collateral curve on the
// wire so the dependency ordering (collateral first) is exercised, and the
// quote_id spelling of the same market must give bit-identical results.
//
// Contract: every FxSpotSpec validation is a 400 naming the field, an unknown
// fx_spot / quote / collateral curve is a 404, a collateral currency outside
// the pair and a collateral curve equal to the curve under construction are
// 400s, and the cache key covers the spot's full content (a spot change is a
// miss) while leaving pure-rates curves untouched.
#include "curve_bootstrapper.h"
#include "curve_cache_key.h"
#include "fx_spot_parser.h"
#include "fx_spot_registry.h"
#include "parity_fixture.h"
#include "term_structure_point_parser.h"

#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/time/calendars/jointcalendar.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedstates.hpp>

#include <functional>
#include <limits>
#include <map>

namespace quantra { namespace testing {

namespace {

// EUR/USD market of the parity case: spot for delivery T+2 on the joint
// TARGET + US settlement calendar, forward points (price units) 1W..1Y.
constexpr double kSpot = 1.0850;
const std::vector<std::pair<QuantLib::Period, double>> kFxPoints = {
    {1 * QuantLib::Weeks, 0.00052},  {1 * QuantLib::Months, 0.00213},
    {3 * QuantLib::Months, 0.00641}, {6 * QuantLib::Months, 0.01310},
    {1 * QuantLib::Years, 0.02702},
};

const std::vector<int8_t> kSettlementCalendars = {
    static_cast<int8_t>(quantra::enums::Calendar_TARGET),
    static_cast<int8_t>(quantra::enums::Calendar_UnitedStatesSettlement)};

QuantLib::Calendar referenceJointCalendar() {
    return QuantLib::JointCalendar(
        QuantLib::TARGET(), QuantLib::UnitedStates(QuantLib::UnitedStates::Settlement),
        QuantLib::JoinHolidays);
}

flatbuffers::Offset<quantra::Period> makePeriod(
    flatbuffers::FlatBufferBuilder& b, const QuantLib::Period& p) {
    quantra::enums::TimeUnit unit = quantra::enums::TimeUnit_Days;
    switch (p.units()) {
        case QuantLib::Weeks: unit = quantra::enums::TimeUnit_Weeks; break;
        case QuantLib::Months: unit = quantra::enums::TimeUnit_Months; break;
        case QuantLib::Years: unit = quantra::enums::TimeUnit_Years; break;
        default: break;
    }
    quantra::PeriodBuilder pb(b);
    pb.add_n(p.length());
    pb.add_unit(unit);
    return pb.Finish();
}

flatbuffers::Offset<quantra::FxSpotSpec> makeEurUsdSpot(
    flatbuffers::FlatBufferBuilder& b, double rate = kSpot, const char* quoteId = nullptr) {
    return quantra::CreateFxSpotSpecDirect(
        b, "EURUSD_SPOT", "EUR", "USD",
        quoteId ? flatbuffers::Optional<double>() : flatbuffers::Optional<double>(rate), quoteId, 2,
        &kSettlementCalendars, quantra::enums::BusinessDayConvention_ModifiedFollowing);
}

flatbuffers::Offset<quantra::PointsWrapper> makeFxPillar(
    flatbuffers::FlatBufferBuilder& b,
    const QuantLib::Period& tenor,
    flatbuffers::Optional<double> points,
    const char* quoteId,
    const char* collateralCcy = "USD",
    const char* collateralCurve = "USD_COLLATERAL",
    const char* spotId = "EURUSD_SPOT") {
    auto tenorOff = makePeriod(b, tenor);
    auto curveRef = quantra::CreateCurveRefDirect(b, collateralCurve);
    auto helper = quantra::CreateFxSwapHelperDirect(
        b, points, quoteId, tenorOff, spotId, false, collateralCcy, curveRef);
    return quantra::CreatePointsWrapper(b, quantra::Point_FxSwapHelper, helper.Union());
}

flatbuffers::Offset<quantra::TermStructure> makeFxCurve(
    flatbuffers::FlatBufferBuilder& b,
    const char* id,
    bool byQuoteId,
    const char* collateralCurve = "USD_COLLATERAL") {
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts;
    for (size_t i = 0; i < kFxPoints.size(); ++i) {
        const std::string quoteId = "EURUSD_PTS_" + std::to_string(i);
        pts.push_back(makeFxPillar(
            b, kFxPoints[i].first,
            byQuoteId ? flatbuffers::Optional<double>()
                      : flatbuffers::Optional<double>(kFxPoints[i].second),
            byQuoteId ? quoteId.c_str() : nullptr, "USD", collateralCurve));
    }
    return quantra::CreateTermStructureDirect(
        b, id, quantra::enums::DayCounter_Actual365Fixed, quantra::enums::Interpolator_LogLinear,
        quantra::enums::BootstrapTrait_Discount, &pts, "2025-01-15");
}

// The QuantLib twin of makeFxCurve: FxSwapRateHelper pillars on the given
// collateral curve, LogLinear discount, Actual365Fixed.
std::shared_ptr<QuantLib::YieldTermStructure> referenceFxCurve(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& collateral, double spot = kSpot) {
    auto spotQuote =
        QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot));
    std::vector<std::shared_ptr<QuantLib::RateHelper>> helpers;
    for (const auto& [tenor, points] : kFxPoints) {
        helpers.push_back(
            std::make_shared<QuantLib::FxSwapRateHelper>(
                QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(points)),
                spotQuote, tenor, 2, referenceJointCalendar(), QuantLib::ModifiedFollowing, false,
                /*isFxBaseCurrencyCollateralCurrency=*/false, collateral));
    }
    auto curve =
        std::make_shared<QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
            QuantLib::Date(15, QuantLib::January, 2025), helpers, QuantLib::Actual365Fixed());
    curve->enableExtrapolation();
    return curve;
}

} // namespace

// =============================================================================
// Parity through the curve-sampling endpoint
// =============================================================================

class FxSwapHelperParityTest : public QuantraComparisonTest {
protected:
    // Sample the EUR USD-collateralised curve on the server, inline or by
    // quote_id, returning {grid date -> DF}.
    std::map<QuantLib::Date, double> sampleFxCurve(bool byQuoteId) {
        flatbuffers::grpc::MessageBuilder b;

        std::vector<flatbuffers::Offset<quantra::Period>> tenors;
        for (const auto& [tenor, _] : kFxPoints)
            tenors.push_back(makePeriod(b, tenor));
        tenors.push_back(makePeriod(b, 9 * QuantLib::Months)); // off-pillar
        auto tenorsVec = b.CreateVector(tenors);
        auto tenorGrid = quantra::CreateTenorGrid(
            b, tenorsVec, quantra::enums::Calendar_TARGET,
            quantra::enums::BusinessDayConvention_ModifiedFollowing);
        auto gridSpec =
            quantra::CreateDateGridSpec(b, quantra::DateGrid_TenorGrid, tenorGrid.Union());
        std::vector<int8_t> measures = {static_cast<int8_t>(quantra::CurveMeasure_DF)};
        auto measuresVec = b.CreateVector(measures);
        auto curveId = b.CreateString("EUR_USD_COLLATERAL");
        quantra::CurveQuerySpecBuilder cqb(b);
        cqb.add_curve_id(curveId);
        cqb.add_measures(measuresVec);
        cqb.add_grid(gridSpec);
        std::vector<flatbuffers::Offset<quantra::CurveQuerySpec>> queries{cqb.Finish()};
        auto queriesVec = b.CreateVector(queries);

        // The FX curve is listed FIRST: the bootstrapper must build its
        // collateral curve before it regardless of wire order.
        std::vector<flatbuffers::Offset<quantra::TermStructure>> curves;
        curves.push_back(makeFxCurve(b, "EUR_USD_COLLATERAL", byQuoteId));
        curves.push_back(buildCurve(b, "USD_COLLATERAL"));
        auto curvesVec = b.CreateVector(curves);
        auto indices = buildIndicesVector(b);

        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>> quotes =
            0;
        if (byQuoteId) {
            std::vector<flatbuffers::Offset<quantra::QuoteSpec>> qs;
            for (size_t i = 0; i < kFxPoints.size(); ++i) {
                const std::string id = "EURUSD_PTS_" + std::to_string(i);
                qs.push_back(
                    quantra::CreateQuoteSpecDirect(
                        b, id.c_str(), quantra::QuoteKind_FxPoints, kFxPoints[i].second,
                        quantra::QuoteType_Curve));
            }
            quotes = b.CreateVector(qs);
        }

        std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{makeEurUsdSpot(b)};
        auto fx = quantra::CreateFxMarketDataDirect(b, &spots);
        auto rates = quantra::CreateRatesMarketData(b, indices, 0, curvesVec, 0);
        auto asOf = b.CreateString("2025-01-15");
        auto pricing = quantra::CreatePricing(b, asOf, 0, quotes, rates, 0, 0, 0, 0, 0, 0, fx);

        quantra::BootstrapCurvesRequestBuilder reqb(b);
        reqb.add_pricing(pricing);
        reqb.add_queries(queriesVec);
        b.Finish(reqb.Finish());

        auto request = flatbuffers::GetRoot<quantra::BootstrapCurvesRequest>(b.GetBufferPointer());
        BootstrapCurvesRequestHandler handler;
        auto rb = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto off = handler.request(rb, request);
        rb->Finish(off);
        auto response =
            flatbuffers::GetRoot<quantra::BootstrapCurvesResponse>(rb->GetBufferPointer());

        EXPECT_EQ(response->results()->size(), 1u);
        auto result = response->results()->Get(0);
        EXPECT_EQ(result->error(), nullptr) << result->error()->error_message()->str();
        EXPECT_EQ(result->series()->size(), 1u);
        auto series = result->series()->Get(0);
        EXPECT_EQ(series->measure(), quantra::CurveMeasure_DF);
        EXPECT_EQ(series->values()->size(), result->grid_dates()->size());

        std::map<QuantLib::Date, double> out;
        for (flatbuffers::uoffset_t i = 0; i < result->grid_dates()->size(); ++i) {
            out[DateToQL(result->grid_dates()->Get(i)->str())] = series->values()->Get(i);
        }
        return out;
    }
};

TEST_F(FxSwapHelperParityTest, UsdCollateralisedEurCurve_MatchesQuantLib) {
    std::cout << "\n--- Test: FxSwapHelper USD-collateralised EUR curve vs QuantLib ---\n";
    auto sampled = sampleFxCurve(/*byQuoteId=*/false);
    ASSERT_EQ(sampled.size(), kFxPoints.size() + 1);

    auto reference = referenceFxCurve(discountHandle_);
    for (const auto& [date, df] : sampled) {
        const double qlDf = reference->discount(date);
        std::cout << "  " << date << ": QL=" << std::setprecision(15) << qlDf << " Quantra=" << df
                  << " diff=" << std::scientific << std::abs(qlDf - df) << std::endl;
        EXPECT_NEAR(df, qlDf, 1e-12);
    }
    // The curve really is FX-implied: at 1Y the EUR DF must differ from the
    // collateral (USD) DF by the forward/spot ratio, not coincide with it.
    const QuantLib::Date oneYear = sampled.rbegin()->first;
    EXPECT_GT(std::abs(reference->discount(oneYear) - discountHandle_->discount(oneYear)), 1e-4);
}

TEST_F(FxSwapHelperParityTest, QuoteIdPillars_IdenticalToInline) {
    auto inlineDfs = sampleFxCurve(/*byQuoteId=*/false);
    auto quoteDfs = sampleFxCurve(/*byQuoteId=*/true);
    ASSERT_EQ(inlineDfs.size(), quoteDfs.size());
    for (const auto& [date, df] : inlineDfs) {
        ASSERT_TRUE(quoteDfs.count(date));
        EXPECT_EQ(quoteDfs.at(date), df) << date;
    }
}

// =============================================================================
// FxSpotSpec parser contract
// =============================================================================

namespace {

struct SpotFields {
    const char* id = "EURUSD_SPOT";
    const char* base = "EUR";
    const char* quote = "USD";
    flatbuffers::Optional<double> rate = flatbuffers::Optional<double>(kSpot);
    const char* quoteId = nullptr;
    flatbuffers::Optional<int32_t> spotDays = flatbuffers::Optional<int32_t>(2);
    const std::vector<int8_t>* calendars = &kSettlementCalendars;
    flatbuffers::Optional<quantra::enums::BusinessDayConvention> bdc =
        flatbuffers::Optional<quantra::enums::BusinessDayConvention>(
            quantra::enums::BusinessDayConvention_ModifiedFollowing);
};

FxSpot parseSpot(const SpotFields& f, const QuoteRegistry* quotes = nullptr) {
    flatbuffers::FlatBufferBuilder b;
    auto spec = quantra::CreateFxSpotSpecDirect(
        b, f.id, f.base, f.quote, f.rate, f.quoteId, f.spotDays, f.calendars, f.bdc);
    b.Finish(spec);
    FxSpotParser parser;
    return parser.parse(flatbuffers::GetRoot<quantra::FxSpotSpec>(b.GetBufferPointer()), quotes);
}

std::string errorOf(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const QuantraError& e) {
        return e.what();
    }
    return "";
}

} // namespace

TEST(FxSpotParserContract, ValidSpotParsesToDomain) {
    FxSpot spot = parseSpot(SpotFields{});
    EXPECT_EQ(spot.id, "EURUSD_SPOT");
    EXPECT_EQ(spot.baseCurrency, "EUR");
    EXPECT_EQ(spot.quoteCurrency, "USD");
    EXPECT_DOUBLE_EQ(spot.rate->value(), kSpot);
    EXPECT_EQ(spot.spotDays, 2);
    EXPECT_EQ(spot.businessDayConvention, QuantLib::ModifiedFollowing);
    // Joint calendar: a US-only holiday is a holiday of the pair.
    EXPECT_FALSE(spot.settlementCalendar.isBusinessDay(QuantLib::Date(4, QuantLib::July, 2025)));
    EXPECT_FALSE(spot.settlementCalendar.isBusinessDay(QuantLib::Date(1, QuantLib::May, 2025)));
    EXPECT_TRUE(spot.settlementCalendar.isBusinessDay(QuantLib::Date(2, QuantLib::May, 2025)));
}

TEST(FxSpotParserContract, SingleCalendarIsUsedDirectly) {
    std::vector<int8_t> only{static_cast<int8_t>(quantra::enums::Calendar_TARGET)};
    SpotFields f;
    f.calendars = &only;
    FxSpot spot = parseSpot(f);
    EXPECT_TRUE(spot.settlementCalendar.isBusinessDay(QuantLib::Date(4, QuantLib::July, 2025)));
}

// A required string can only be ABSENT over JSON (the FlatBuffers builder
// asserts on it; the gateway rejects it with "required field is missing").
// The C++ contract therefore covers the empty-string spelling.
TEST(FxSpotParserContract, MissingIdRejected) {
    SpotFields f;
    f.id = "";
    EXPECT_THROW(parseSpot(f), QuantraInvalidArgument);
    EXPECT_NE(errorOf([&] { parseSpot(f); }).find("FxSpotSpec.id"), std::string::npos);
}

TEST(FxSpotParserContract, SameBaseAndQuoteRejected) {
    SpotFields f;
    f.quote = "EUR";
    EXPECT_THROW(parseSpot(f), QuantraInvalidArgument);
    EXPECT_NE(errorOf([&] { parseSpot(f); }).find("must differ"), std::string::npos);
}

TEST(FxSpotParserContract, CurrencyCodeShapeEnforced) {
    SpotFields lower;
    lower.base = "eur";
    EXPECT_THROW(parseSpot(lower), QuantraInvalidArgument);
    SpotFields pair;
    pair.quote = "EURUSD";
    EXPECT_THROW(parseSpot(pair), QuantraInvalidArgument);
    EXPECT_NE(
        errorOf([&] { parseSpot(pair); }).find("FxSpotSpec.quote_currency"), std::string::npos);
    SpotFields missing;
    missing.base = "";
    EXPECT_THROW(parseSpot(missing), QuantraInvalidArgument);
}

TEST(FxSpotParserContract, RateXorQuoteId) {
    SpotFields neither;
    neither.rate = flatbuffers::Optional<double>();
    EXPECT_THROW(parseSpot(neither), QuantraInvalidArgument);
    EXPECT_NE(
        errorOf([&] { parseSpot(neither); }).find("exactly one of rate or quote_id"),
        std::string::npos);

    QuoteRegistry quotes;
    quotes.upsert("EURUSD", kSpot, quantra::QuoteType_Fx);
    SpotFields both;
    both.quoteId = "EURUSD";
    EXPECT_THROW(parseSpot(both, &quotes), QuantraInvalidArgument);
}

TEST(FxSpotParserContract, RateMustBePositiveAndFinite) {
    SpotFields zero;
    zero.rate = flatbuffers::Optional<double>(0.0);
    EXPECT_THROW(parseSpot(zero), QuantraInvalidArgument);
    SpotFields negative;
    negative.rate = flatbuffers::Optional<double>(-1.08);
    EXPECT_THROW(parseSpot(negative), QuantraInvalidArgument);
    SpotFields nan;
    nan.rate = flatbuffers::Optional<double>(std::numeric_limits<double>::quiet_NaN());
    EXPECT_THROW(parseSpot(nan), QuantraInvalidArgument);
    EXPECT_NE(errorOf([&] { parseSpot(zero); }).find("FxSpotSpec.rate"), std::string::npos);
}

TEST(FxSpotParserContract, QuoteIdResolvesFxQuotesOnly) {
    QuoteRegistry quotes;
    quotes.upsert("EURUSD_FX", kSpot, quantra::QuoteType_Fx);
    quotes.upsert("EURUSD_CURVE", kSpot, quantra::QuoteType_Curve);

    SpotFields ok;
    ok.rate = flatbuffers::Optional<double>();
    ok.quoteId = "EURUSD_FX";
    FxSpot spot = parseSpot(ok, &quotes);
    EXPECT_DOUBLE_EQ(spot.rate->value(), kSpot);
    // The handle observes the registry quote (bump-and-reprice relies on it).
    EXPECT_EQ(spot.rate.currentLink(), quotes.getHandle("EURUSD_FX").currentLink());

    // A Curve-typed quote would be shifted by the rate rebump: wrong kind -> 400.
    SpotFields wrongKind = ok;
    wrongKind.quoteId = "EURUSD_CURVE";
    EXPECT_THROW(parseSpot(wrongKind, &quotes), QuantraInvalidArgument);
    EXPECT_NE(
        errorOf([&] { parseSpot(wrongKind, &quotes); }).find("quote_type Fx"), std::string::npos);

    // Unknown quote id -> 404, same as a pillar's unknown quote id.
    SpotFields unknown = ok;
    unknown.quoteId = "NO_SUCH_QUOTE";
    EXPECT_THROW(parseSpot(unknown, &quotes), QuantraNotFound);
}

TEST(FxSpotParserContract, SpotDaysPresenceAndSign) {
    SpotFields absent;
    absent.spotDays = flatbuffers::Optional<int32_t>();
    EXPECT_THROW(parseSpot(absent), QuantraInvalidArgument);
    EXPECT_NE(errorOf([&] { parseSpot(absent); }).find("FxSpotSpec.spot_days"), std::string::npos);
    SpotFields negative;
    negative.spotDays = flatbuffers::Optional<int32_t>(-1);
    EXPECT_THROW(parseSpot(negative), QuantraInvalidArgument);
    SpotFields sameDay;
    sameDay.spotDays = flatbuffers::Optional<int32_t>(0);
    EXPECT_EQ(parseSpot(sameDay).spotDays, 0);
}

TEST(FxSpotParserContract, SettlementCalendarsRequiredNonEmpty) {
    std::vector<int8_t> none;
    SpotFields empty;
    empty.calendars = &none;
    EXPECT_THROW(parseSpot(empty), QuantraInvalidArgument);
    EXPECT_NE(
        errorOf([&] { parseSpot(empty); }).find("FxSpotSpec.settlement_calendars"),
        std::string::npos);
}

TEST(FxSpotParserContract, BusinessDayConventionRequired) {
    SpotFields f;
    f.bdc = flatbuffers::Optional<quantra::enums::BusinessDayConvention>();
    EXPECT_THROW(parseSpot(f), QuantraInvalidArgument);
    EXPECT_NE(
        errorOf([&] { parseSpot(f); }).find("FxSpotSpec.business_day_convention"),
        std::string::npos);
}

TEST(FxSpotRegistryContract, DuplicateIdRejectedAndLookupIs404) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{
        makeEurUsdSpot(b), makeEurUsdSpot(b, 1.09)};
    b.Finish(quantra::CreateFxMarketDataDirect(b, &spots));
    const auto* fx = flatbuffers::GetRoot<quantra::FxMarketData>(b.GetBufferPointer());

    QuoteRegistry quotes;
    FxSpotRegistryBuilder builder;
    EXPECT_THROW(builder.build(fx, quotes), QuantraInvalidArgument);

    flatbuffers::FlatBufferBuilder b2;
    std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> one{makeEurUsdSpot(b2)};
    b2.Finish(quantra::CreateFxMarketDataDirect(b2, &one));
    FxSpotRegistry reg =
        builder.build(flatbuffers::GetRoot<quantra::FxMarketData>(b2.GetBufferPointer()), quotes);
    EXPECT_EQ(reg.size(), 1u);
    EXPECT_TRUE(reg.has("EURUSD_SPOT"));
    EXPECT_THROW(reg.get("GBPUSD_SPOT"), QuantraNotFound);
    EXPECT_EQ(builder.build(nullptr, quotes).size(), 0u);
}

// =============================================================================
// FxSwapHelper point parser contract
// =============================================================================

namespace {

struct FxHelperEnv {
    QuoteRegistry quotes;
    CurveRegistry curves;
    IndexRegistry indices;
    FxSpotRegistry spots;
    QuantLib::RelinkableHandle<QuantLib::YieldTermStructure> usd;

    FxHelperEnv() {
        usd.linkTo(
            std::make_shared<QuantLib::FlatForward>(
                QuantLib::Date(15, QuantLib::January, 2025), 0.03, QuantLib::Actual365Fixed()));
        curves.put("USD_COLLATERAL", usd);
        spots.put(parseSpot(SpotFields{}));
        quotes.upsert("PTS_1M", 0.00213, quantra::QuoteType_Curve);
    }

    std::shared_ptr<QuantLib::RateHelper> parse(
        flatbuffers::Optional<double> points,
        const char* quoteId,
        const char* collateralCcy = "USD",
        const char* collateralCurve = "USD_COLLATERAL",
        const char* spotId = "EURUSD_SPOT",
        double bump = 0.0) {
        flatbuffers::FlatBufferBuilder b;
        auto pw = makeFxPillar(
            b, 1 * QuantLib::Months, points, quoteId, collateralCcy, collateralCurve, spotId);
        b.Finish(pw);
        const auto* wrapper = flatbuffers::GetRoot<quantra::PointsWrapper>(b.GetBufferPointer());
        TermStructurePointParser parser;
        return parser.parse(
            wrapper->point_type(), wrapper->point(), &quotes, &curves, &indices, bump, &spots);
    }
};

} // namespace

TEST(FxSwapHelperContract, BuildsQuantLibHelperFromSpotConventions) {
    FxHelperEnv env;
    auto helper = env.parse(0.00213, nullptr);
    auto fx = std::dynamic_pointer_cast<QuantLib::FxSwapRateHelper>(helper);
    ASSERT_TRUE(fx);
    EXPECT_DOUBLE_EQ(fx->spot(), kSpot);
    EXPECT_DOUBLE_EQ(fx->quote()->value(), 0.00213);
    EXPECT_EQ(fx->fixingDays(), 2u);
    EXPECT_EQ(fx->tenor(), 1 * QuantLib::Months);
    EXPECT_EQ(fx->businessDayConvention(), QuantLib::ModifiedFollowing);
    EXPECT_FALSE(fx->endOfMonth());
    // USD is the quote currency of EUR/USD -> base is NOT the collateral.
    EXPECT_FALSE(fx->isFxBaseCurrencyCollateralCurrency());
    EXPECT_EQ(fx->calendar().name(), referenceJointCalendar().name());

    auto eurColl =
        std::dynamic_pointer_cast<QuantLib::FxSwapRateHelper>(env.parse(0.00213, nullptr, "EUR"));
    EXPECT_TRUE(eurColl->isFxBaseCurrencyCollateralCurrency());
}

TEST(FxSwapHelperContract, PointsXorQuoteId) {
    FxHelperEnv env;
    EXPECT_THROW(env.parse(flatbuffers::Optional<double>(), nullptr), QuantraInvalidArgument);
    EXPECT_THROW(env.parse(0.00213, "PTS_1M"), QuantraInvalidArgument);
    auto byQuote = std::dynamic_pointer_cast<QuantLib::FxSwapRateHelper>(
        env.parse(flatbuffers::Optional<double>(), "PTS_1M"));
    ASSERT_TRUE(byQuote);
    EXPECT_DOUBLE_EQ(byQuote->quote()->value(), 0.00213);
    // Forward points are curve quotes: an unknown id is a 404, a bump on the
    // inline spelling shifts the points.
    EXPECT_THROW(env.parse(flatbuffers::Optional<double>(), "NO_SUCH"), QuantraNotFound);
    auto bumped = std::dynamic_pointer_cast<QuantLib::FxSwapRateHelper>(
        env.parse(0.00213, nullptr, "USD", "USD_COLLATERAL", "EURUSD_SPOT", 1e-4));
    EXPECT_DOUBLE_EQ(bumped->quote()->value(), 0.00213 + 1e-4);
}

TEST(FxSwapHelperContract, CollateralCurrencyMustBeInPair) {
    FxHelperEnv env;
    EXPECT_THROW(env.parse(0.00213, nullptr, "GBP"), QuantraInvalidArgument);
    const std::string msg = errorOf([&] { env.parse(0.00213, nullptr, "GBP"); });
    EXPECT_NE(msg.find("FxSwapHelper.collateral_currency"), std::string::npos);
    EXPECT_NE(msg.find("EUR/USD"), std::string::npos);
    EXPECT_THROW(env.parse(0.00213, nullptr, ""), QuantraInvalidArgument);
}

TEST(FxSwapHelperContract, UnknownReferencesAre404) {
    FxHelperEnv env;
    EXPECT_THROW(
        env.parse(0.00213, nullptr, "USD", "USD_COLLATERAL", "GBPUSD_SPOT"), QuantraNotFound);
    EXPECT_NE(
        errorOf([&] {
            env.parse(0.00213, nullptr, "USD", "USD_COLLATERAL", "GBPUSD_SPOT");
        }).find("GBPUSD_SPOT"),
        std::string::npos);
    EXPECT_THROW(env.parse(0.00213, nullptr, "USD", "NO_SUCH_CURVE"), QuantraNotFound);
    EXPECT_THROW(env.parse(0.00213, nullptr, "USD", ""), QuantraInvalidArgument);
    EXPECT_NE(
        errorOf([&] {
            env.parse(0.00213, nullptr, "USD", "");
        }).find("FxSwapHelper.collateral_curve.id is required"),
        std::string::npos);
}

TEST(FxSwapHelperContract, TenorBasisSwapHelperStillNotImplemented) {
    // CrossCcyBasisHelper is built now (cross_ccy_basis_helper_test.cpp); the
    // tenor basis helper is the only schema-ready-but-unbuilt pillar left.
    flatbuffers::FlatBufferBuilder b;
    auto tenor = makePeriod(b, 5 * QuantLib::Years);
    auto shortIdx = quantra::CreateIndexRefDirect(b, "EUR_3M");
    auto longIdx = quantra::CreateIndexRefDirect(b, "EUR_6M");
    auto helper = quantra::CreateTenorBasisSwapHelperDirect(
        b, 0.0005, tenor, shortIdx, longIdx, quantra::enums::Calendar_TARGET);
    b.Finish(quantra::CreatePointsWrapper(b, quantra::Point_TenorBasisSwapHelper, helper.Union()));
    const auto* wrapper = flatbuffers::GetRoot<quantra::PointsWrapper>(b.GetBufferPointer());
    FxHelperEnv env;
    TermStructurePointParser parser;
    EXPECT_THROW(
        parser.parse(
            wrapper->point_type(), wrapper->point(), &env.quotes, &env.curves, &env.indices, 0.0,
            &env.spots),
        QuantraNotImplemented);
}

// =============================================================================
// Dependency ordering through the bootstrapper
// =============================================================================

namespace {

// pricing.rates.curves = [FX curve (collateral = collateralRef), USD curve]
// with the EUR/USD spot; returns the wire vectors for bootstrapAll.
struct FxBootstrapRequest {
    flatbuffers::FlatBufferBuilder b;
    const quantra::Pricing* pricing = nullptr;

    explicit FxBootstrapRequest(const char* collateralRef, bool includeSpot = true) {
        // A flat deposit-only USD curve is enough for the ordering checks; it
        // must reach past the 1Y FX pillar (spot T+2, then +1Y), hence 24M.
        std::vector<flatbuffers::Offset<quantra::PointsWrapper>> usdPts;
        for (int months : {3, 6, 12, 24}) {
            auto tenor = makePeriod(b, months * QuantLib::Months);
            auto dep = quantra::CreateDepositHelper(
                b, 0.03, tenor, 2, quantra::enums::Calendar_TARGET,
                quantra::enums::BusinessDayConvention_ModifiedFollowing,
                quantra::enums::DayCounter_Actual365Fixed);
            usdPts.push_back(
                quantra::CreatePointsWrapper(b, quantra::Point_DepositHelper, dep.Union()));
        }
        auto usd = quantra::CreateTermStructureDirect(
            b, "USD_COLLATERAL", quantra::enums::DayCounter_Actual365Fixed,
            quantra::enums::Interpolator_LogLinear, quantra::enums::BootstrapTrait_Discount,
            &usdPts, "2025-01-15");
        std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
            makeFxCurve(b, "EUR_USD_COLLATERAL", false, collateralRef), usd};
        auto curvesVec = b.CreateVector(curves);
        auto rates = quantra::CreateRatesMarketData(b, 0, 0, curvesVec, 0);
        flatbuffers::Offset<quantra::FxMarketData> fx = 0;
        if (includeSpot) {
            std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{makeEurUsdSpot(b)};
            fx = quantra::CreateFxMarketDataDirect(b, &spots);
        }
        auto asOf = b.CreateString("2025-01-15");
        b.Finish(quantra::CreatePricing(b, asOf, 0, 0, rates, 0, 0, 0, 0, 0, 0, fx));
        pricing = flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer());
    }

    BootstrappedCurves bootstrap() const {
        QuantLib::Settings::instance().evaluationDate() =
            QuantLib::Date(15, QuantLib::January, 2025);
        CurveBootstrapper bootstrapper;
        return bootstrapper.bootstrapAll(
            pricing->rates()->curves(), pricing->quotes(), pricing->rates()->indices(), 0.0,
            RequestBudget::unlimited(), pricing->fx());
    }
};

} // namespace

TEST(FxSwapHelperDependencies, CollateralCurveIsBuiltFirstWhateverTheWireOrder) {
    FxBootstrapRequest req("USD_COLLATERAL");
    auto booted = req.bootstrap();
    ASSERT_TRUE(booted.handles.count("EUR_USD_COLLATERAL"));
    ASSERT_TRUE(booted.handles.count("USD_COLLATERAL"));
    const auto& eur = *booted.handles.at("EUR_USD_COLLATERAL");
    const auto& usd = *booted.handles.at("USD_COLLATERAL");
    const QuantLib::Date d(15, QuantLib::July, 2025);
    EXPECT_GT(eur->discount(d), 0.0);
    EXPECT_LT(eur->discount(d), 1.0);
    // EUR rates implied by positive EUR/USD points sit BELOW the USD curve.
    EXPECT_GT(eur->discount(d), usd->discount(d));
}

TEST(FxSwapHelperDependencies, UnknownCollateralCurveIs404) {
    FxBootstrapRequest req("NO_SUCH_CURVE");
    EXPECT_THROW(req.bootstrap(), QuantraNotFound);
}

TEST(FxSwapHelperDependencies, SelfReferencingCollateralCurveIs400) {
    FxBootstrapRequest req("EUR_USD_COLLATERAL");
    EXPECT_THROW(req.bootstrap(), QuantraInvalidArgument);
    EXPECT_NE(
        errorOf([&] { req.bootstrap(); }).find("is the curve being bootstrapped"),
        std::string::npos);
}

TEST(FxSwapHelperDependencies, MissingSpotIs404) {
    FxBootstrapRequest req("USD_COLLATERAL", /*includeSpot=*/false);
    EXPECT_THROW(req.bootstrap(), QuantraNotFound);
}

// =============================================================================
// Cache key: the spot's full content is part of the FX curve's key
// =============================================================================

namespace {

std::string fxCurveKey(
    double spotRate,
    int spotDays = 2,
    const std::vector<int8_t>& calendars = kSettlementCalendars,
    quantra::enums::BusinessDayConvention bdc =
        quantra::enums::BusinessDayConvention_ModifiedFollowing,
    const char* spotQuoteId = nullptr,
    quantra::QuoteType spotQuoteType = quantra::QuoteType_Fx) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{quantra::CreateFxSpotSpecDirect(
        b, "EURUSD_SPOT", "EUR", "USD",
        spotQuoteId ? flatbuffers::Optional<double>() : flatbuffers::Optional<double>(spotRate),
        spotQuoteId, spotDays, &calendars, bdc)};
    auto fx = quantra::CreateFxMarketDataDirect(b, &spots);
    std::vector<flatbuffers::Offset<quantra::QuoteSpec>> qs;
    if (spotQuoteId) {
        qs.push_back(
            quantra::CreateQuoteSpecDirect(
                b, spotQuoteId, quantra::QuoteKind_FxSpot, spotRate, spotQuoteType));
    }
    auto quotes = b.CreateVector(qs);
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
        makeFxCurve(b, "EUR_USD_COLLATERAL", false)};
    auto curvesVec = b.CreateVector(curves);
    auto rates = quantra::CreateRatesMarketData(b, 0, 0, curvesVec, 0);
    auto asOf = b.CreateString("2025-01-15");
    b.Finish(quantra::CreatePricing(b, asOf, 0, quotes, rates, 0, 0, 0, 0, 0, 0, fx));
    const auto* pricing = flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer());

    KeyContext ctx = KeyContext::build(pricing->quotes(), nullptr, pricing->fx());
    std::map<std::string, std::string> depKeys{{"USD_COLLATERAL", "yc:v3:dep"}};
    return CurveKeyBuilder::compute("2025-01-15", pricing->rates()->curves()->Get(0), ctx, depKeys);
}

} // namespace

TEST(FxSwapHelperCacheKey, SpotContentChangesTheKey) {
    const std::string base = fxCurveKey(kSpot);
    EXPECT_EQ(base.rfind("yc:v3:", 0), 0u);
    EXPECT_EQ(fxCurveKey(kSpot), base) << "deterministic";
    EXPECT_NE(fxCurveKey(1.0851), base) << "spot rate";
    EXPECT_NE(fxCurveKey(kSpot, 1), base) << "spot days";
    std::vector<int8_t> targetOnly{static_cast<int8_t>(quantra::enums::Calendar_TARGET)};
    EXPECT_NE(fxCurveKey(kSpot, 2, targetOnly), base) << "calendars";
    EXPECT_NE(
        fxCurveKey(kSpot, 2, kSettlementCalendars, quantra::enums::BusinessDayConvention_Following),
        base)
        << "convention";
    // Calendar order does not matter: the joint calendar is the same.
    std::vector<int8_t> swapped{kSettlementCalendars[1], kSettlementCalendars[0]};
    EXPECT_EQ(fxCurveKey(kSpot, 2, swapped), base);
}

TEST(FxSwapHelperCacheKey, SpotQuoteIdResolvesFxQuoteOrFailsClosed) {
    // The quote_id spelling keys on the resolved rate, so two different Fx
    // quote values must not collide, and an unresolvable id fails closed.
    const std::string a = fxCurveKey(
        kSpot, 2, kSettlementCalendars, quantra::enums::BusinessDayConvention_ModifiedFollowing,
        "EURUSD");
    const std::string b = fxCurveKey(
        1.0851, 2, kSettlementCalendars, quantra::enums::BusinessDayConvention_ModifiedFollowing,
        "EURUSD");
    EXPECT_NE(a, b);
    // A Curve-typed quote is not an Fx quote: the parser rejects the spot, so
    // the key builder must throw rather than key the curve.
    EXPECT_THROW(
        fxCurveKey(
            kSpot, 2, kSettlementCalendars, quantra::enums::BusinessDayConvention_ModifiedFollowing,
            "EURUSD", quantra::QuoteType_Curve),
        QuantraError);
}

TEST(FxSwapHelperCacheKey, UnknownSpotFailsClosed) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
        makeFxCurve(b, "EUR_USD_COLLATERAL", false)};
    b.Finish(curves[0]);
    const auto* ts = flatbuffers::GetRoot<quantra::TermStructure>(b.GetBufferPointer());
    KeyContext ctx; // no spots: EURUSD_SPOT resolves to nothing
    EXPECT_THROW(CurveKeyBuilder::compute("2025-01-15", ts, ctx, {}), QuantraError);
}

TEST(FxSwapHelperCacheKey, PureRatesCurveIgnoresFxSpots) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts;
    auto tenor = makePeriod(b, 6 * QuantLib::Months);
    auto dep = quantra::CreateDepositHelper(
        b, 0.03, tenor, 2, quantra::enums::Calendar_TARGET,
        quantra::enums::BusinessDayConvention_ModifiedFollowing,
        quantra::enums::DayCounter_Actual365Fixed);
    pts.push_back(quantra::CreatePointsWrapper(b, quantra::Point_DepositHelper, dep.Union()));
    auto ts = quantra::CreateTermStructureDirect(
        b, "USD_COLLATERAL", quantra::enums::DayCounter_Actual365Fixed,
        quantra::enums::Interpolator_LogLinear, quantra::enums::BootstrapTrait_Discount, &pts,
        "2025-01-15");
    b.Finish(ts);
    const auto* root = flatbuffers::GetRoot<quantra::TermStructure>(b.GetBufferPointer());

    flatbuffers::FlatBufferBuilder fb;
    std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{makeEurUsdSpot(fb)};
    fb.Finish(quantra::CreateFxMarketDataDirect(fb, &spots));
    KeyContext withSpot = KeyContext::build(
        nullptr, nullptr, flatbuffers::GetRoot<quantra::FxMarketData>(fb.GetBufferPointer()));
    KeyContext without;
    EXPECT_EQ(
        CurveKeyBuilder::compute("2025-01-15", root, withSpot, {}),
        CurveKeyBuilder::compute("2025-01-15", root, without, {}));
}

}} // namespace quantra::testing
