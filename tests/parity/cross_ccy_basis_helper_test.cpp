// Cross-currency basis helper (CrossCcyBasisHelper) parity and contract tests.
//
// Parity: a USD-collateralised EUR discount curve bootstrapped through the
// curve-sampling endpoint from FX swap points (short end) and constant-
// notional EUR/USD basis swaps (long end) must reproduce, discount factor by
// discount factor, a QuantLib PiecewiseYieldCurve built directly from
// FxSwapRateHelper + ConstNotionalCrossCurrencyBasisSwapRateHelper on the
// same spot, points, spreads, indices and exogenous curves. Both leg shapes
// QuantLib offers are covered: two IBOR indices paying at their tenors (no
// payment frequency on the wire) and an overnight quote leg paying compounded
// quarterly coupons (payment_frequency required). The curve is listed before
// every curve it depends on, and the quote_id spelling must be bit-identical.
//
// Contract: every CrossCcyBasisHelper validation is a 400 naming the field,
// unknown references are 404s, a reference back to the curve under
// construction is a 400, the spot rate does not change a constant-notional
// bootstrap although it is part of the cache key, and every field of the
// helper is part of the key.
#include "curve_bootstrapper.h"
#include "curve_cache_key.h"
#include "fx_spot_parser.h"
#include "fx_spot_registry.h"
#include "parity_fixture.h"
#include "term_structure_point_parser.h"

#include <ql/experimental/termstructures/crosscurrencyratehelpers.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/time/calendars/jointcalendar.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/calendars/unitedstates.hpp>

#include <algorithm>
#include <functional>
#include <map>

namespace quantra { namespace testing {

namespace {

// =============================================================================
// Market of the parity case
// =============================================================================

constexpr double kSpot = 1.0850;
constexpr double kUsdDepositRate = 0.045;
const QuantLib::Date kAsOf(15, QuantLib::January, 2025);

// EUR/USD forward points (price units), spot T+2 on TARGET + US settlement.
const std::vector<std::pair<QuantLib::Period, double>> kFxPoints = {
    {1 * QuantLib::Weeks, 0.00052},  {1 * QuantLib::Months, 0.00213},
    {3 * QuantLib::Months, 0.00641}, {6 * QuantLib::Months, 0.01310},
    {1 * QuantLib::Years, 0.02702},
};
// Constant-notional basis on the EUR leg, negative as in the market.
const std::vector<std::pair<QuantLib::Period, double>> kBasis = {
    {2 * QuantLib::Years, -0.0012},
    {3 * QuantLib::Years, -0.0014},
    {5 * QuantLib::Years, -0.0017},
};
// Flat USD deposit curve reaching past the 5Y pillar's last coupon.
const std::vector<QuantLib::Period> kUsdDepositTenors = {
    3 * QuantLib::Months, 6 * QuantLib::Months, 1 * QuantLib::Years, 2 * QuantLib::Years,
    3 * QuantLib::Years,  5 * QuantLib::Years,  6 * QuantLib::Years,
};

const std::vector<int8_t> kSettlementCalendars = {
    static_cast<int8_t>(quantra::enums::Calendar_TARGET),
    static_cast<int8_t>(quantra::enums::Calendar_UnitedStatesSettlement)};

/// Which index the quote (USD) leg carries: a 3M IBOR index (legs pay at the
/// index tenor, no payment frequency) or SOFR (compounded quarterly leg).
enum class QuoteLeg { Ibor3M, Sofr };

const char* quoteIndexId(QuoteLeg leg) {
    return leg == QuoteLeg::Ibor3M ? "USD_3M" : "USD_SOFR";
}

QuantLib::Calendar usCalendar() {
    return QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond);
}

QuantLib::Calendar jointSettlementCalendar() {
    return QuantLib::JointCalendar(
        QuantLib::TARGET(), QuantLib::UnitedStates(QuantLib::UnitedStates::Settlement),
        QuantLib::JoinHolidays);
}

// =============================================================================
// Wire builders
// =============================================================================

flatbuffers::Offset<quantra::Period> makePeriod(
    flatbuffers::FlatBufferBuilder& b, const QuantLib::Period& p) {
    quantra::enums::TimeUnit unit = quantra::enums::TimeUnit_Days;
    switch (p.units()) {
        case QuantLib::Weeks: unit = quantra::enums::TimeUnit_Weeks; break;
        case QuantLib::Months: unit = quantra::enums::TimeUnit_Months; break;
        case QuantLib::Years: unit = quantra::enums::TimeUnit_Years; break;
        default: break;
    }
    return quantra::CreatePeriod(b, p.length(), unit);
}

flatbuffers::Offset<quantra::FxSpotSpec> makeEurUsdSpot(
    flatbuffers::FlatBufferBuilder& b, double rate = kSpot) {
    return quantra::CreateFxSpotSpecDirect(
        b, "EURUSD_SPOT", "EUR", "USD", rate, nullptr, 2, &kSettlementCalendars,
        quantra::enums::BusinessDayConvention_ModifiedFollowing);
}

/// IndexDef for a generic USD 3M IBOR index (term-rate conventions).
flatbuffers::Offset<quantra::IndexDef> makeUsd3MIndexDef(flatbuffers::FlatBufferBuilder& b) {
    auto tenor = makePeriod(b, 3 * QuantLib::Months);
    return quantra::CreateIndexDefDirect(
        b, "USD_3M", "USD3M", quantra::IndexType_Ibor, tenor, 2,
        quantra::enums::Calendar_UnitedStatesGovernmentBond,
        quantra::enums::BusinessDayConvention_ModifiedFollowing,
        quantra::enums::DayCounter_Actual360, false, "USD");
}

/// IndexDef for EUR 3M (the fixture's EUR_3M conventions) and USD SOFR (the
/// fixture's USD_SOFR conventions), usable outside the fixture.
flatbuffers::Offset<quantra::IndexDef> makeEur3MIndexDef(flatbuffers::FlatBufferBuilder& b) {
    auto tenor = makePeriod(b, 3 * QuantLib::Months);
    return quantra::CreateIndexDefDirect(
        b, "EUR_3M", "Euribor", quantra::IndexType_Ibor, tenor, 2, quantra::enums::Calendar_TARGET,
        quantra::enums::BusinessDayConvention_ModifiedFollowing,
        quantra::enums::DayCounter_Actual360, false, "EUR");
}

flatbuffers::Offset<quantra::IndexDef> makeSofrIndexDef(flatbuffers::FlatBufferBuilder& b) {
    auto tenor = makePeriod(b, 0 * QuantLib::Days);
    return quantra::CreateIndexDefDirect(
        b, "USD_SOFR", "SOFR", quantra::IndexType_Overnight, tenor, 0,
        quantra::enums::Calendar_UnitedStatesGovernmentBond,
        quantra::enums::BusinessDayConvention_Following, quantra::enums::DayCounter_Actual360, true,
        "USD");
}

flatbuffers::Offset<quantra::PointsWrapper> makeFxPillar(
    flatbuffers::FlatBufferBuilder& b, const QuantLib::Period& tenor, double points) {
    auto tenorOff = makePeriod(b, tenor);
    auto curveRef = quantra::CreateCurveRefDirect(b, "USD_COLLATERAL");
    auto helper = quantra::CreateFxSwapHelperDirect(
        b, points, nullptr, tenorOff, "EURUSD_SPOT", false, "USD", curveRef);
    return quantra::CreatePointsWrapper(b, quantra::Point_FxSwapHelper, helper.Union());
}

/// Every field of a CrossCcyBasisHelper, defaulted to the parity case: EUR_3M
/// vs the chosen USD index, EUR forwarding off EUR_FWD, USD forwarding and
/// discounting off USD_COLLATERAL, basis on the EUR leg.
struct XccyFields {
    flatbuffers::Optional<double> spread = flatbuffers::Optional<double>(-0.0012);
    const char* quoteId = nullptr;
    QuantLib::Period tenor = 2 * QuantLib::Years;
    const char* spotId = "EURUSD_SPOT";
    const char* baseIndex = "EUR_3M";
    const char* baseForwarding = "EUR_FWD";
    const char* baseDiscount = nullptr;
    const char* quoteIndex = "USD_3M";
    const char* quoteForwarding = "USD_COLLATERAL";
    const char* quoteDiscount = "USD_COLLATERAL";
    flatbuffers::Optional<bool> spreadOnBase = flatbuffers::Optional<bool>(true);
    flatbuffers::Optional<quantra::enums::Frequency> paymentFrequency =
        flatbuffers::Optional<quantra::enums::Frequency>();
    flatbuffers::Optional<int32_t> paymentLag = flatbuffers::Optional<int32_t>(0);
    flatbuffers::Optional<quantra::enums::Calendar> calendar =
        flatbuffers::Optional<quantra::enums::Calendar>(quantra::enums::Calendar_TARGET);
    flatbuffers::Optional<quantra::enums::BusinessDayConvention> bdc =
        flatbuffers::Optional<quantra::enums::BusinessDayConvention>(
            quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flatbuffers::Optional<bool> endOfMonth = flatbuffers::Optional<bool>(false);
    flatbuffers::Optional<bool> mtmReset = flatbuffers::Optional<bool>(false);

    static XccyFields forQuoteLeg(QuoteLeg leg) {
        XccyFields f;
        f.quoteIndex = quoteIndexId(leg);
        if (leg == QuoteLeg::Sofr) {
            f.paymentFrequency = flatbuffers::Optional<quantra::enums::Frequency>(
                quantra::enums::Frequency_Quarterly);
        }
        return f;
    }
};

flatbuffers::Offset<quantra::PointsWrapper> makeXccyPillar(
    flatbuffers::FlatBufferBuilder& b, const XccyFields& f) {
    auto tenor = makePeriod(b, f.tenor);
    auto ref = [&](const char* id) {
        return id ? quantra::CreateCurveRefDirect(b, id) : flatbuffers::Offset<quantra::CurveRef>();
    };
    auto helper = quantra::CreateCrossCcyBasisHelperDirect(
        b, f.spread, f.quoteId, tenor, f.spotId, quantra::CreateIndexRefDirect(b, f.baseIndex),
        ref(f.baseForwarding), ref(f.baseDiscount), quantra::CreateIndexRefDirect(b, f.quoteIndex),
        ref(f.quoteForwarding), ref(f.quoteDiscount), f.spreadOnBase, f.paymentFrequency,
        f.paymentLag, f.calendar, f.bdc, f.endOfMonth, f.mtmReset);
    return quantra::CreatePointsWrapper(b, quantra::Point_CrossCcyBasisHelper, helper.Union());
}

flatbuffers::Offset<quantra::TermStructure> makeDiscountCurve(
    flatbuffers::FlatBufferBuilder& b,
    const char* id,
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>>& pts) {
    return quantra::CreateTermStructureDirect(
        b, id, quantra::enums::DayCounter_Actual365Fixed, quantra::enums::Interpolator_LogLinear,
        quantra::enums::BootstrapTrait_Discount, &pts, "2025-01-15");
}

/// The curve under test: FX swap pillars (optional) + basis pillars whose
/// fields are `mutate`d from the parity defaults.
flatbuffers::Offset<quantra::TermStructure> makeCollateralCurve(
    flatbuffers::FlatBufferBuilder& b,
    QuoteLeg leg,
    bool withFxSwapPillars = true,
    const std::function<void(XccyFields&, size_t)>& mutate = {}) {
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts;
    if (withFxSwapPillars) {
        for (const auto& [tenor, points] : kFxPoints)
            pts.push_back(makeFxPillar(b, tenor, points));
    }
    for (size_t i = 0; i < kBasis.size(); ++i) {
        XccyFields f = XccyFields::forQuoteLeg(leg);
        f.tenor = kBasis[i].first;
        f.spread = flatbuffers::Optional<double>(kBasis[i].second);
        if (mutate) mutate(f, i);
        pts.push_back(makeXccyPillar(b, f));
    }
    return makeDiscountCurve(b, "EUR_USD_COLLATERAL", pts);
}

/// A flat deposit curve (DepositHelper pillars at `rate`).
flatbuffers::Offset<quantra::TermStructure> makeDepositCurve(
    flatbuffers::FlatBufferBuilder& b, const char* id, double rate) {
    std::vector<flatbuffers::Offset<quantra::PointsWrapper>> pts;
    for (const auto& tenor : kUsdDepositTenors) {
        auto t = makePeriod(b, tenor);
        auto dep = quantra::CreateDepositHelper(
            b, rate, t, 2, quantra::enums::Calendar_TARGET,
            quantra::enums::BusinessDayConvention_ModifiedFollowing,
            quantra::enums::DayCounter_Actual365Fixed);
        pts.push_back(quantra::CreatePointsWrapper(b, quantra::Point_DepositHelper, dep.Union()));
    }
    return makeDiscountCurve(b, id, pts);
}

flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>> makeIndices(
    flatbuffers::FlatBufferBuilder& b) {
    std::vector<flatbuffers::Offset<quantra::IndexDef>> defs{
        makeEur3MIndexDef(b), makeSofrIndexDef(b), makeUsd3MIndexDef(b)};
    return b.CreateVector(defs);
}

// =============================================================================
// QuantLib twins
// =============================================================================

std::shared_ptr<QuantLib::YieldTermStructure> referenceDepositCurve(double rate) {
    std::vector<std::shared_ptr<QuantLib::RateHelper>> helpers;
    for (const auto& tenor : kUsdDepositTenors) {
        helpers.push_back(
            std::make_shared<QuantLib::DepositRateHelper>(
                rate, tenor, 2, QuantLib::TARGET(), QuantLib::ModifiedFollowing, true,
                QuantLib::Actual365Fixed()));
    }
    auto curve =
        std::make_shared<QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
            kAsOf, helpers, QuantLib::Actual365Fixed());
    curve->enableExtrapolation();
    return curve;
}

std::shared_ptr<QuantLib::IborIndex> referenceEur3M(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding) {
    return std::make_shared<QuantLib::IborIndex>(
        "Euribor", 3 * QuantLib::Months, 2, QuantLib::EURCurrency(), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, false, QuantLib::Actual360(), forwarding);
}

std::shared_ptr<QuantLib::IborIndex> referenceQuoteIndex(
    QuoteLeg leg, const QuantLib::Handle<QuantLib::YieldTermStructure>& forwarding) {
    if (leg == QuoteLeg::Ibor3M) {
        return std::make_shared<QuantLib::IborIndex>(
            "USD3M", 3 * QuantLib::Months, 2, QuantLib::USDCurrency(), usCalendar(),
            QuantLib::ModifiedFollowing, false, QuantLib::Actual360(), forwarding);
    }
    return std::make_shared<QuantLib::OvernightIndex>(
        "SOFR", 0, QuantLib::USDCurrency(), usCalendar(), QuantLib::Actual360(), forwarding);
}

/// The twin of makeCollateralCurve: FxSwapRateHelper + constant-notional
/// basis helpers on the reference EUR forwarding and USD curves.
std::shared_ptr<QuantLib::YieldTermStructure> referenceCollateralCurve(
    QuoteLeg leg,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& eurForwarding,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& usd,
    bool spreadOnBase = true) {
    auto spotQuote =
        QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(kSpot));
    std::vector<std::shared_ptr<QuantLib::RateHelper>> helpers;
    for (const auto& [tenor, points] : kFxPoints) {
        helpers.push_back(
            std::make_shared<QuantLib::FxSwapRateHelper>(
                QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(points)),
                spotQuote, tenor, 2, jointSettlementCalendar(), QuantLib::ModifiedFollowing, false,
                /*isFxBaseCurrencyCollateralCurrency=*/false, usd));
    }
    auto eur3m = referenceEur3M(eurForwarding);
    auto quoteIdx = referenceQuoteIndex(leg, usd);
    const QuantLib::Frequency frequency =
        leg == QuoteLeg::Sofr ? QuantLib::Quarterly : QuantLib::NoFrequency;
    for (const auto& [tenor, basis] : kBasis) {
        helpers.push_back(
            std::make_shared<QuantLib::ConstNotionalCrossCurrencyBasisSwapRateHelper>(
                QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(basis)),
                tenor, 2, QuantLib::TARGET(), QuantLib::ModifiedFollowing, false, eur3m, quoteIdx,
                usd, /*isFxBaseCurrencyCollateralCurrency=*/false, spreadOnBase, frequency, 0));
    }
    auto curve =
        std::make_shared<QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
            kAsOf, helpers, QuantLib::Actual365Fixed());
    curve->enableExtrapolation();
    return curve;
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

// =============================================================================
// Parity through the curve-sampling endpoint
// =============================================================================

class CrossCcyBasisHelperParityTest : public QuantraComparisonTest {
protected:
    struct Options {
        QuoteLeg leg = QuoteLeg::Ibor3M;
        bool byQuoteId = false;
        bool reverseWireOrder = false;
        bool spreadOnBase = true;
    };

    // Sample EUR_USD_COLLATERAL on the server: {grid date -> DF}. The EUR
    // forwarding curve is the fixture's bootstrapped curve (its QuantLib twin
    // is discountHandle_), the USD curve a flat deposit curve.
    std::map<QuantLib::Date, double> sampleCurve(const Options& o) {
        flatbuffers::grpc::MessageBuilder b;

        std::vector<flatbuffers::Offset<quantra::Period>> tenors;
        for (const auto& [tenor, _] : kFxPoints)
            tenors.push_back(makePeriod(b, tenor));
        for (const auto& [tenor, _] : kBasis)
            tenors.push_back(makePeriod(b, tenor));
        tenors.push_back(makePeriod(b, 4 * QuantLib::Years)); // off-pillar
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

        static const std::vector<std::string> kQuoteIds = {
            "EURUSD_XCCY_0", "EURUSD_XCCY_1", "EURUSD_XCCY_2"};
        auto mutate = [&](XccyFields& f, size_t i) {
            f.spreadOnBase = flatbuffers::Optional<bool>(o.spreadOnBase);
            if (o.byQuoteId) {
                f.spread = flatbuffers::Optional<double>();
                f.quoteId = kQuoteIds.at(i).c_str();
            }
        };
        // The collateral curve is listed FIRST although it depends on both
        // other curves; reversed order must give the same curve.
        std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
            makeCollateralCurve(b, o.leg, true, mutate), buildCurve(b, "EUR_FWD"),
            makeDepositCurve(b, "USD_COLLATERAL", kUsdDepositRate)};
        if (o.reverseWireOrder) std::reverse(curves.begin(), curves.end());
        auto curvesVec = b.CreateVector(curves);

        std::vector<flatbuffers::Offset<quantra::IndexDef>> defs{
            buildIndexDef_EUR6M(b), buildIndexDef_EUR3M(b), buildIndexDef_USD_SOFR(b),
            makeUsd3MIndexDef(b)};
        auto indices = b.CreateVector(defs);

        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>> quotes =
            0;
        if (o.byQuoteId) {
            std::vector<flatbuffers::Offset<quantra::QuoteSpec>> qs;
            for (size_t i = 0; i < kBasis.size(); ++i) {
                const std::string id = "EURUSD_XCCY_" + std::to_string(i);
                qs.push_back(
                    quantra::CreateQuoteSpecDirect(
                        b, id.c_str(), quantra::QuoteKind_Spread, kBasis[i].second,
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
        EXPECT_EQ(series->values()->size(), result->grid_dates()->size());

        std::map<QuantLib::Date, double> out;
        for (flatbuffers::uoffset_t i = 0; i < result->grid_dates()->size(); ++i) {
            out[DateToQL(result->grid_dates()->Get(i)->str())] = series->values()->Get(i);
        }
        return out;
    }

    void expectParity(QuoteLeg leg) {
        auto sampled = sampleCurve(Options{leg});
        ASSERT_EQ(sampled.size(), kFxPoints.size() + kBasis.size() + 1);
        QuantLib::Handle<QuantLib::YieldTermStructure> usd(referenceDepositCurve(kUsdDepositRate));
        auto reference = referenceCollateralCurve(leg, discountHandle_, usd);
        for (const auto& [date, df] : sampled) {
            const double qlDf = reference->discount(date);
            std::cout << "  " << date << ": QL=" << std::setprecision(15) << qlDf
                      << " Quantra=" << df << " diff=" << std::scientific << std::abs(qlDf - df)
                      << std::endl;
            EXPECT_NEAR(df, qlDf, 1e-12);
        }
        // Genuinely a different curve from the USD one at the long end.
        const QuantLib::Date fiveYears = sampled.rbegin()->first;
        EXPECT_GT(std::abs(reference->discount(fiveYears) - usd->discount(fiveYears)), 1e-4);
    }
};

TEST_F(CrossCcyBasisHelperParityTest, IborLegs_MatchQuantLib) {
    std::cout << "\n--- Test: CrossCcyBasisHelper EUR 3M vs USD 3M (no payment frequency) ---\n";
    expectParity(QuoteLeg::Ibor3M);
}

TEST_F(CrossCcyBasisHelperParityTest, OvernightQuoteLeg_MatchesQuantLib) {
    std::cout << "\n--- Test: CrossCcyBasisHelper EUR 3M vs USD SOFR (quarterly) ---\n";
    expectParity(QuoteLeg::Sofr);
}

TEST_F(CrossCcyBasisHelperParityTest, QuoteIdPillars_IdenticalToInline) {
    auto inlineDfs = sampleCurve(Options{QuoteLeg::Sofr, /*byQuoteId=*/false});
    auto quoteDfs = sampleCurve(Options{QuoteLeg::Sofr, /*byQuoteId=*/true});
    ASSERT_EQ(inlineDfs.size(), quoteDfs.size());
    for (const auto& [date, df] : inlineDfs) {
        ASSERT_TRUE(quoteDfs.count(date));
        EXPECT_EQ(quoteDfs.at(date), df) << date;
    }
}

TEST_F(CrossCcyBasisHelperParityTest, WireOrderOfCurvesDoesNotMatter) {
    Options reversed{QuoteLeg::Sofr};
    reversed.reverseWireOrder = true;
    auto first = sampleCurve(Options{QuoteLeg::Sofr});
    auto second = sampleCurve(reversed);
    ASSERT_EQ(first.size(), second.size());
    for (const auto& [date, df] : first)
        EXPECT_EQ(second.at(date), df) << date;
}

TEST_F(CrossCcyBasisHelperParityTest, SpreadOnQuoteLegIsADifferentCurve) {
    Options onQuote{QuoteLeg::Sofr};
    onQuote.spreadOnBase = false;
    auto onBase = sampleCurve(Options{QuoteLeg::Sofr});
    auto other = sampleCurve(onQuote);
    const QuantLib::Date fiveYears = onBase.rbegin()->first;
    EXPECT_NE(onBase.at(fiveYears), other.at(fiveYears));
    // And the flipped flag matches QuantLib with the same flag.
    QuantLib::Handle<QuantLib::YieldTermStructure> usd(referenceDepositCurve(kUsdDepositRate));
    auto reference = referenceCollateralCurve(QuoteLeg::Sofr, discountHandle_, usd, false);
    for (const auto& [date, df] : other)
        EXPECT_NEAR(df, reference->discount(date), 1e-12);
}

// =============================================================================
// CrossCcyBasisHelper point parser contract
// =============================================================================

namespace {

struct XccyEnv {
    QuoteRegistry quotes;
    CurveRegistry curves;
    IndexRegistry indices;
    FxSpotRegistry spots;
    QuantLib::RelinkableHandle<QuantLib::YieldTermStructure> eur, usd;

    XccyEnv() {
        eur.linkTo(
            std::make_shared<QuantLib::FlatForward>(kAsOf, 0.025, QuantLib::Actual365Fixed()));
        usd.linkTo(
            std::make_shared<QuantLib::FlatForward>(kAsOf, 0.045, QuantLib::Actual365Fixed()));
        curves.put("EUR_FWD", eur);
        curves.put("USD_COLLATERAL", usd);
        indices.put("EUR_3M", referenceEur3M(QuantLib::Handle<QuantLib::YieldTermStructure>()));
        indices.put(
            "USD_3M", referenceQuoteIndex(
                          QuoteLeg::Ibor3M, QuantLib::Handle<QuantLib::YieldTermStructure>()));
        indices.put(
            "USD_SOFR",
            referenceQuoteIndex(QuoteLeg::Sofr, QuantLib::Handle<QuantLib::YieldTermStructure>()));
        flatbuffers::FlatBufferBuilder b;
        b.Finish(makeEurUsdSpot(b));
        FxSpotParser parser;
        spots.put(
            parser.parse(flatbuffers::GetRoot<quantra::FxSpotSpec>(b.GetBufferPointer()), nullptr));
        quotes.upsert("XCCY_2Y", -0.0012, quantra::QuoteType_Curve);
    }

    std::shared_ptr<QuantLib::RateHelper> parse(const XccyFields& f, double bump = 0.0) {
        flatbuffers::FlatBufferBuilder b;
        b.Finish(makeXccyPillar(b, f));
        const auto* wrapper = flatbuffers::GetRoot<quantra::PointsWrapper>(b.GetBufferPointer());
        TermStructurePointParser parser;
        return parser.parse(
            wrapper->point_type(), wrapper->point(), &quotes, &curves, &indices, bump, &spots);
    }

    std::string error(const XccyFields& f) {
        return errorOf([&] { parse(f); });
    }
};

} // namespace

TEST(CrossCcyBasisHelperContract, BuildsQuantLibHelper) {
    XccyEnv env;
    auto helper = env.parse(XccyFields::forQuoteLeg(QuoteLeg::Ibor3M));
    auto xccy =
        std::dynamic_pointer_cast<QuantLib::ConstNotionalCrossCurrencyBasisSwapRateHelper>(helper);
    ASSERT_TRUE(xccy);
    EXPECT_DOUBLE_EQ(xccy->quote()->value(), -0.0012);
    // Settlement = as-of + 2 TARGET days (spot lag on the helper calendar),
    // pillar = settlement + 2Y adjusted (MF), no payment lag.
    EXPECT_EQ(xccy->earliestDate(), QuantLib::Date(17, QuantLib::January, 2025));
    EXPECT_EQ(xccy->pillarDate(), QuantLib::Date(18, QuantLib::January, 2027));

    auto sofr = std::dynamic_pointer_cast<QuantLib::ConstNotionalCrossCurrencyBasisSwapRateHelper>(
        env.parse(XccyFields::forQuoteLeg(QuoteLeg::Sofr)));
    ASSERT_TRUE(sofr);

    // The basis is a curve quote: the bump shifts an inline spread.
    auto bumped =
        std::dynamic_pointer_cast<QuantLib::ConstNotionalCrossCurrencyBasisSwapRateHelper>(
            env.parse(XccyFields::forQuoteLeg(QuoteLeg::Ibor3M), 1e-4));
    EXPECT_DOUBLE_EQ(bumped->quote()->value(), -0.0012 + 1e-4);

    // A payment lag moves the pillar to the last payment date.
    XccyFields lagged = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
    lagged.paymentLag = flatbuffers::Optional<int32_t>(2);
    EXPECT_EQ(env.parse(lagged)->pillarDate(), QuantLib::Date(20, QuantLib::January, 2027));
}

TEST(CrossCcyBasisHelperContract, SpreadXorQuoteId) {
    XccyEnv env;
    XccyFields neither = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
    neither.spread = flatbuffers::Optional<double>();
    EXPECT_THROW(env.parse(neither), QuantraInvalidArgument);
    EXPECT_NE(env.error(neither).find("exactly one of spread or quote_id"), std::string::npos);

    XccyFields both = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
    both.quoteId = "XCCY_2Y";
    EXPECT_THROW(env.parse(both), QuantraInvalidArgument);

    XccyFields byQuote = neither;
    byQuote.quoteId = "XCCY_2Y";
    EXPECT_DOUBLE_EQ(env.parse(byQuote)->quote()->value(), -0.0012);

    XccyFields unknown = neither;
    unknown.quoteId = "NO_SUCH_QUOTE";
    EXPECT_THROW(env.parse(unknown), QuantraNotFound);
}

TEST(CrossCcyBasisHelperContract, IndexCurrencyMustMatchTheSpot) {
    XccyEnv env;
    XccyFields f = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
    f.baseIndex = "USD_3M"; // a USD index on the EUR leg
    EXPECT_THROW(env.parse(f), QuantraInvalidArgument);
    const std::string msg = env.error(f);
    EXPECT_NE(msg.find("CrossCcyBasisHelper.base_index"), std::string::npos);
    EXPECT_NE(msg.find("base_currency"), std::string::npos);
    EXPECT_NE(msg.find("EUR"), std::string::npos);

    XccyFields g = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
    g.quoteIndex = "EUR_3M";
    EXPECT_NE(env.error(g).find("CrossCcyBasisHelper.quote_index"), std::string::npos);
}

TEST(CrossCcyBasisHelperContract, ExactlyOneDiscountCurve) {
    XccyEnv env;
    XccyFields both = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
    both.baseDiscount = "EUR_FWD";
    EXPECT_THROW(env.parse(both), QuantraInvalidArgument);
    EXPECT_NE(
        env.error(both).find("exactly one of base_discount_curve or quote_discount_curve"),
        std::string::npos);
    XccyFields neither = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
    neither.quoteDiscount = nullptr;
    EXPECT_THROW(env.parse(neither), QuantraInvalidArgument);
    // EUR collateral instead: well-formed, flips the collateral side.
    XccyFields eurColl = neither;
    eurColl.baseDiscount = "EUR_FWD";
    EXPECT_TRUE(env.parse(eurColl));
}

TEST(CrossCcyBasisHelperContract, PaymentFrequencyRules) {
    XccyEnv env;
    // Overnight leg without a frequency: QuantLib cannot build the leg.
    XccyFields noFreq = XccyFields::forQuoteLeg(QuoteLeg::Sofr);
    noFreq.paymentFrequency = flatbuffers::Optional<quantra::enums::Frequency>();
    EXPECT_THROW(env.parse(noFreq), QuantraInvalidArgument);
    EXPECT_NE(env.error(noFreq).find("CrossCcyBasisHelper.payment_frequency"), std::string::npos);
    // Two IBOR legs with a frequency: nothing to choose, reject.
    XccyFields iborFreq = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
    iborFreq.paymentFrequency =
        flatbuffers::Optional<quantra::enums::Frequency>(quantra::enums::Frequency_Quarterly);
    EXPECT_THROW(env.parse(iborFreq), QuantraInvalidArgument);
    EXPECT_NE(env.error(iborFreq).find("must be absent"), std::string::npos);
    // Mixed legs: the frequency must equal the IBOR index tenor.
    XccyFields mismatch = XccyFields::forQuoteLeg(QuoteLeg::Sofr);
    mismatch.paymentFrequency =
        flatbuffers::Optional<quantra::enums::Frequency>(quantra::enums::Frequency_Semiannual);
    EXPECT_THROW(env.parse(mismatch), QuantraInvalidArgument);
    EXPECT_NE(env.error(mismatch).find("does not match the tenor"), std::string::npos);
    // A non-periodic frequency is meaningless.
    XccyFields once = XccyFields::forQuoteLeg(QuoteLeg::Sofr);
    once.paymentFrequency =
        flatbuffers::Optional<quantra::enums::Frequency>(quantra::enums::Frequency_Once);
    EXPECT_THROW(env.parse(once), QuantraInvalidArgument);
}

TEST(CrossCcyBasisHelperContract, PresenceRequiredFieldsAndMtmReset) {
    XccyEnv env;
    auto expect400 = [&](const std::function<void(XccyFields&)>& mutate, const char* fragment) {
        XccyFields f = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
        mutate(f);
        EXPECT_THROW(env.parse(f), QuantraInvalidArgument) << fragment;
        EXPECT_NE(env.error(f).find(fragment), std::string::npos) << env.error(f);
    };
    expect400(
        [](XccyFields& f) { f.spreadOnBase = flatbuffers::Optional<bool>(); },
        "CrossCcyBasisHelper.spread_on_base_leg is required");
    expect400(
        [](XccyFields& f) { f.paymentLag = flatbuffers::Optional<int32_t>(); },
        "CrossCcyBasisHelper.payment_lag");
    expect400(
        [](XccyFields& f) { f.paymentLag = flatbuffers::Optional<int32_t>(-1); },
        "CrossCcyBasisHelper.payment_lag");
    expect400(
        [](XccyFields& f) { f.calendar = flatbuffers::Optional<quantra::enums::Calendar>(); },
        "CrossCcyBasisHelper.calendar is required");
    expect400(
        [](XccyFields& f) {
            f.bdc = flatbuffers::Optional<quantra::enums::BusinessDayConvention>();
        },
        "CrossCcyBasisHelper.business_day_convention is required");
    expect400(
        [](XccyFields& f) { f.endOfMonth = flatbuffers::Optional<bool>(); },
        "CrossCcyBasisHelper.end_of_month is required");
    expect400(
        [](XccyFields& f) { f.mtmReset = flatbuffers::Optional<bool>(); },
        "CrossCcyBasisHelper.mtm_reset is required");
    expect400(
        [](XccyFields& f) { f.mtmReset = flatbuffers::Optional<bool>(true); },
        "not supported in this version");
    expect400([](XccyFields& f) { f.spotId = ""; }, "CrossCcyBasisHelper.fx_spot is required");
    expect400(
        [](XccyFields& f) { f.baseIndex = ""; }, "CrossCcyBasisHelper.base_index.id is required");
    expect400(
        [](XccyFields& f) { f.quoteForwarding = ""; },
        "CrossCcyBasisHelper.quote_forwarding_curve.id is required");
}

TEST(CrossCcyBasisHelperContract, UnknownReferencesAre404) {
    XccyEnv env;
    auto expect404 = [&](const std::function<void(XccyFields&)>& mutate, const char* fragment) {
        XccyFields f = XccyFields::forQuoteLeg(QuoteLeg::Ibor3M);
        mutate(f);
        EXPECT_THROW(env.parse(f), QuantraNotFound) << fragment;
        EXPECT_NE(env.error(f).find(fragment), std::string::npos) << env.error(f);
    };
    expect404([](XccyFields& f) { f.spotId = "GBPUSD_SPOT"; }, "FX spot not found: GBPUSD_SPOT");
    expect404([](XccyFields& f) { f.baseIndex = "EUR_12M"; }, "EUR_12M");
    expect404([](XccyFields& f) { f.quoteIndex = "USD_6M"; }, "USD_6M");
    expect404([](XccyFields& f) { f.baseForwarding = "NO_SUCH_CURVE"; }, "NO_SUCH_CURVE");
    expect404([](XccyFields& f) { f.quoteDiscount = "NO_SUCH_CURVE"; }, "NO_SUCH_CURVE");
}

// =============================================================================
// Dependency ordering and the constant-notional property (bootstrapper)
// =============================================================================

namespace {

/// pricing.rates.curves = [collateral curve, EUR_FWD, USD_COLLATERAL] (flat
/// deposit curves for the two exogenous ones), indices, EUR/USD spot.
struct XccyBootstrapRequest {
    flatbuffers::FlatBufferBuilder b;
    const quantra::Pricing* pricing = nullptr;

    XccyBootstrapRequest(
        const std::function<void(XccyFields&, size_t)>& mutate,
        bool withFxSwapPillars = true,
        double spotRate = kSpot) {
        std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
            makeCollateralCurve(b, QuoteLeg::Sofr, withFxSwapPillars, mutate),
            makeDepositCurve(b, "EUR_FWD", 0.025),
            makeDepositCurve(b, "USD_COLLATERAL", kUsdDepositRate)};
        auto curvesVec = b.CreateVector(curves);
        auto rates = quantra::CreateRatesMarketData(b, makeIndices(b), 0, curvesVec, 0);
        std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{makeEurUsdSpot(b, spotRate)};
        auto fx = quantra::CreateFxMarketDataDirect(b, &spots);
        auto asOf = b.CreateString("2025-01-15");
        b.Finish(quantra::CreatePricing(b, asOf, 0, 0, rates, 0, 0, 0, 0, 0, 0, fx));
        pricing = flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer());
    }

    BootstrappedCurves bootstrap() const {
        QuantLib::Settings::instance().evaluationDate() = kAsOf;
        CurveBootstrapper bootstrapper;
        return bootstrapper.bootstrapAll(
            pricing->rates()->curves(), pricing->quotes(), pricing->rates()->indices(), 0.0,
            RequestBudget::unlimited(), pricing->fx());
    }
};

} // namespace

TEST(CrossCcyBasisHelperDependencies, ExogenousCurvesAreBuiltFirstWhateverTheWireOrder) {
    XccyBootstrapRequest req({});
    auto booted = req.bootstrap();
    ASSERT_TRUE(booted.handles.count("EUR_USD_COLLATERAL"));
    const auto& eur = *booted.handles.at("EUR_USD_COLLATERAL");
    const auto& usd = *booted.handles.at("USD_COLLATERAL");
    const QuantLib::Date d(17, QuantLib::January, 2028);
    EXPECT_GT(eur->discount(d), 0.0);
    EXPECT_LT(eur->discount(d), 1.0);
    EXPECT_NE(eur->discount(d), usd->discount(d));
}

TEST(CrossCcyBasisHelperDependencies, SelfReferencesAre400) {
    auto check = [](const std::function<void(XccyFields&)>& mutate, const char* field) {
        XccyBootstrapRequest req([&](XccyFields& f, size_t) { mutate(f); });
        EXPECT_THROW(req.bootstrap(), QuantraInvalidArgument) << field;
        const std::string msg = errorOf([&] { req.bootstrap(); });
        EXPECT_NE(msg.find(field), std::string::npos) << msg;
        EXPECT_NE(msg.find("is the curve being bootstrapped"), std::string::npos) << msg;
    };
    check([](XccyFields& f) { f.baseForwarding = "EUR_USD_COLLATERAL"; }, "base_forwarding_curve");
    check(
        [](XccyFields& f) { f.quoteForwarding = "EUR_USD_COLLATERAL"; }, "quote_forwarding_curve");
    check([](XccyFields& f) { f.quoteDiscount = "EUR_USD_COLLATERAL"; }, "quote_discount_curve");
}

TEST(CrossCcyBasisHelperDependencies, BothOrNeitherDiscountCurveIs400BeforeAnyLookup) {
    XccyBootstrapRequest both([](XccyFields& f, size_t) { f.baseDiscount = "NO_SUCH_CURVE"; });
    EXPECT_THROW(both.bootstrap(), QuantraInvalidArgument);
    XccyBootstrapRequest neither([](XccyFields& f, size_t) { f.quoteDiscount = nullptr; });
    EXPECT_THROW(neither.bootstrap(), QuantraInvalidArgument);
}

TEST(CrossCcyBasisHelperDependencies, UnknownDependencyIs404) {
    XccyBootstrapRequest req([](XccyFields& f, size_t) { f.baseForwarding = "NO_SUCH_CURVE"; });
    EXPECT_THROW(req.bootstrap(), QuantraNotFound);
}

TEST(CrossCcyBasisHelperDependencies, SpotRateDoesNotEnterAConstantNotionalBootstrap) {
    // Basis pillars only (no FX swap short end): the spot cancels from the
    // par condition, so two spots give the same curve to the last bit.
    XccyBootstrapRequest a({}, /*withFxSwapPillars=*/false, 1.0850);
    XccyBootstrapRequest b({}, /*withFxSwapPillars=*/false, 1.2500);
    auto bootedA = a.bootstrap();
    auto bootedB = b.bootstrap();
    const auto& curveA = *bootedA.handles.at("EUR_USD_COLLATERAL");
    const auto& curveB = *bootedB.handles.at("EUR_USD_COLLATERAL");
    for (int year : {1, 2, 3, 4, 5}) {
        const QuantLib::Date d = kAsOf + year * QuantLib::Years;
        EXPECT_EQ(curveA->discount(d), curveB->discount(d)) << d;
    }
}

// =============================================================================
// Cache key: every field of the helper and the spot's content are keyed
// =============================================================================

namespace {

std::string xccyCurveKey(
    const std::function<void(XccyFields&, size_t)>& mutate, double spotRate = kSpot) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<quantra::FxSpotSpec>> spots{makeEurUsdSpot(b, spotRate)};
    auto fx = quantra::CreateFxMarketDataDirect(b, &spots);
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
        makeCollateralCurve(b, QuoteLeg::Sofr, false, mutate)};
    auto curvesVec = b.CreateVector(curves);
    auto rates = quantra::CreateRatesMarketData(b, makeIndices(b), 0, curvesVec, 0);
    auto asOf = b.CreateString("2025-01-15");
    b.Finish(quantra::CreatePricing(b, asOf, 0, 0, rates, 0, 0, 0, 0, 0, 0, fx));
    const auto* pricing = flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer());

    KeyContext ctx = KeyContext::build(nullptr, pricing->rates()->indices(), pricing->fx());
    std::map<std::string, std::string> depKeys{
        {"EUR_FWD", "yc:v3:eur"}, {"USD_COLLATERAL", "yc:v3:usd"}};
    return CurveKeyBuilder::compute("2025-01-15", pricing->rates()->curves()->Get(0), ctx, depKeys);
}

} // namespace

TEST(CrossCcyBasisHelperCacheKey, EveryFieldChangesTheKey) {
    const std::string base = xccyCurveKey({});
    EXPECT_EQ(xccyCurveKey({}), base) << "deterministic";
    EXPECT_NE(
        xccyCurveKey([](XccyFields& f, size_t i) {
            if (i == 0) f.spread = flatbuffers::Optional<double>(-0.0013);
        }),
        base)
        << "one basis spread";
    EXPECT_NE(xccyCurveKey({}, 1.0851), base)
        << "spot rate (keyed with the spot's full content although the curve is unchanged)";
    EXPECT_NE(xccyCurveKey([](XccyFields& f, size_t) { f.spreadOnBase = false; }), base)
        << "spread side";
    EXPECT_NE(
        xccyCurveKey([](XccyFields& f, size_t) {
            f.paymentFrequency = flatbuffers::Optional<quantra::enums::Frequency>(
                quantra::enums::Frequency_Semiannual);
        }),
        base)
        << "payment frequency";
    EXPECT_NE(
        xccyCurveKey(
            [](XccyFields& f, size_t) { f.paymentLag = flatbuffers::Optional<int32_t>(2); }),
        base)
        << "payment lag";
    EXPECT_NE(
        xccyCurveKey([](XccyFields& f, size_t) { f.baseForwarding = "USD_COLLATERAL"; }), base)
        << "base forwarding curve ref";
    EXPECT_NE(xccyCurveKey([](XccyFields& f, size_t) { f.quoteForwarding = "EUR_FWD"; }), base)
        << "quote forwarding curve ref";
    EXPECT_NE(
        xccyCurveKey([](XccyFields& f, size_t) {
            f.quoteDiscount = nullptr;
            f.baseDiscount = "EUR_FWD";
        }),
        base)
        << "collateral side";
    EXPECT_NE(xccyCurveKey([](XccyFields& f, size_t) { f.quoteIndex = "USD_3M"; }), base)
        << "quote index";
    EXPECT_NE(xccyCurveKey([](XccyFields& f, size_t) { f.endOfMonth = true; }), base)
        << "end of month";
    EXPECT_NE(
        xccyCurveKey([](XccyFields& f, size_t) {
            f.calendar = flatbuffers::Optional<quantra::enums::Calendar>(
                quantra::enums::Calendar_UnitedStatesGovernmentBond);
        }),
        base)
        << "calendar";
}

TEST(CrossCcyBasisHelperCacheKey, UnknownSpotOrIndexFailsClosed) {
    flatbuffers::FlatBufferBuilder b;
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves{
        makeCollateralCurve(b, QuoteLeg::Sofr, false)};
    b.Finish(curves[0]);
    const auto* ts = flatbuffers::GetRoot<quantra::TermStructure>(b.GetBufferPointer());
    KeyContext empty; // no spot, no index definitions
    EXPECT_THROW(CurveKeyBuilder::compute("2025-01-15", ts, empty, {}), QuantraError);
}

}} // namespace quantra::testing
