// SSVI equity vol surfaces through the wire: POST /calibrate-equity-vol (shape=SsviCalibrate),
// shape=SsviParams validation and pricing, the calibrate cache, and the vol bump used by the
// structured-note greeks on a realistic inverted term structure.
//
// Truth: the "good set" of ssvi_test.cpp, expiries at 91, 182, 365, 730 days after the reference
// date (2025-01-15), spot 100, risk-free 4%, dividend yield 1.5%, quoted as implied vols at nine
// strikes from 60% to 140% of the forward plus one at the forward (the calibration anchor).
#include "date_convert.h"
#include "parity_fixture.h"
#include "ssvi.h"
#include "ssvi_calibrate_cache.h"
#include "vol_surface_parsers.h"

#include <ql/pricingengines/blackformula.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>

#include <cmath>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace quantra { namespace testing {

namespace {

struct SliceIn {
    int days;
    double theta;
    double rho;
    double psi;
    std::optional<double> forward;
};

struct QuoteIn {
    int days;
    double strike;
    bool put = false;
    std::optional<double> vol;
    std::optional<double> price;
    std::optional<double> bid;
    std::optional<double> ask;
};

struct VolIn {
    std::string id = "eq_vol";
    quantra::enums::VolSurfaceShape shape = quantra::enums::VolSurfaceShape_SsviCalibrate;
    std::vector<SliceIn> slices;
    std::vector<QuoteIn> quotes;
    bool withFit = false;
    quantra::enums::SsviQuoteWeighting weighting = quantra::enums::SsviQuoteWeighting_Vega;
    double maxAbsLogMoneyness = 1.0;
    bool noDividend = false; // SsviCalibrate without any dividend curve
    bool useCurves = false;  // curve ids "discount" / "div" instead of the flat rates below
    double riskFree = 0.04;
    double dividend = 0.015;
    double spot = 100.0;
};

const std::vector<SliceIn> kGoodSet = {
    {91, 0.010000, -0.70, 0.10, std::nullopt},
    {182, 0.018050, -0.68, 0.14, std::nullopt},
    {365, 0.034225, -0.65, 0.20, std::nullopt},
    {730, 0.064800, -0.60, 0.28, std::nullopt}};

double yearFraction(int days) {
    return days / 365.0;
}

flatbuffers::Optional<double> opt(const std::optional<double>& v) {
    return v ? flatbuffers::Optional<double>(*v) : flatbuffers::nullopt;
}

// Implied-vol call quotes at 60%..140% of the forward plus one at the forward, from a truth set.
std::vector<QuoteIn> truthQuotes(
    const std::vector<SliceIn>& truth, double spot, double r, double q) {
    std::vector<QuoteIn> out;
    for (const SliceIn& s : truth) {
        const double T = yearFraction(s.days);
        const double F = spot * std::exp((r - q) * T);
        const ssvi::Node n{s.theta, s.psi, s.rho * s.psi, 0, 0, 0};
        std::vector<double> mult{0.6, 0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3, 1.4};
        for (double m : mult) {
            QuoteIn quote;
            quote.days = s.days;
            quote.strike = F * m;
            quote.vol = std::sqrt(ssvi::w(n, std::log(m)) / T);
            out.push_back(quote);
        }
    }
    return out;
}

// Call and put price quotes at 80%..120% of the forward (10 quotes per slice, call then put), from
// a truth set; bid / ask are the price -+ 0.01 when requested.
std::vector<QuoteIn> parityQuotes(
    const std::vector<SliceIn>& truth, double spot, double r, double q, bool bidAsk = false) {
    std::vector<QuoteIn> out;
    for (const SliceIn& s : truth) {
        const double T = yearFraction(s.days);
        const double F = spot * std::exp((r - q) * T);
        const double D = std::exp(-r * T);
        const ssvi::Node n{s.theta, s.psi, s.rho * s.psi, 0, 0, 0};
        for (double m : {0.8, 0.9, 1.0, 1.1, 1.2}) {
            const double sd = std::sqrt(ssvi::w(n, std::log(m)));
            for (bool put : {false, true}) {
                QuoteIn quote;
                quote.days = s.days;
                quote.strike = F * m;
                quote.put = put;
                quote.price = D * QuantLib::blackFormula(
                                      put ? QuantLib::Option::Put : QuantLib::Option::Call,
                                      quote.strike, F, sd);
                if (bidAsk) {
                    quote.bid = *quote.price - 0.01;
                    quote.ask = *quote.price + 0.01;
                }
                out.push_back(quote);
            }
        }
    }
    return out;
}

class CalibrateEquityVolTest : public QuantraComparisonTest {
protected:
    void SetUp() override {
        QuantraComparisonTest::SetUp();
        SsviCalibrateCache::instance().clear();
        SsviCalibrateCache::setEnabledOverrideForTesting(std::nullopt);
    }
    void TearDown() override {
        SsviCalibrateCache::setEnabledOverrideForTesting(std::nullopt);
        SsviCalibrateCache::instance().clear();
    }

    std::string isoAfter(int days) const { return DateToIso(evaluationDate_ + days); }

    flatbuffers::Offset<quantra::VolSurfaceSpec> buildVol(
        flatbuffers::grpc::MessageBuilder& b, const VolIn& in) {
        auto refDate = b.CreateString("2025-01-15");
        quantra::BlackVolBaseSpecBuilder bb(b);
        bb.add_reference_date(refDate);
        bb.add_calendar(quantra::enums::Calendar_TARGET);
        bb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        bb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        bb.add_shape(in.shape);
        auto base = bb.Finish();

        std::vector<flatbuffers::Offset<quantra::SsviSlice>> slices;
        for (const SliceIn& s : in.slices) {
            slices.push_back(
                quantra::CreateSsviSlice(
                    b, b.CreateString(isoAfter(s.days)), s.theta, s.rho, s.psi, opt(s.forward)));
        }
        std::vector<flatbuffers::Offset<quantra::EquityOptionQuote>> quotes;
        for (const QuoteIn& q : in.quotes) {
            quotes.push_back(
                quantra::CreateEquityOptionQuote(
                    b, b.CreateString(isoAfter(q.days)), q.strike,
                    q.put ? quantra::enums::EquityOptionType_Put
                          : quantra::enums::EquityOptionType_Call,
                    opt(q.price), opt(q.vol), opt(q.bid), opt(q.ask)));
        }
        flatbuffers::Offset<quantra::SsviFitSpec> fit;
        if (in.withFit) {
            quantra::SsviFitSpecBuilder fb(b);
            fb.add_weighting(in.weighting);
            fb.add_max_abs_log_moneyness(in.maxAbsLogMoneyness);
            fit = fb.Finish();
        }

        using SliceVec =
            flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SsviSlice>>>;
        using QuoteVec = flatbuffers::Offset<
            flatbuffers::Vector<flatbuffers::Offset<quantra::EquityOptionQuote>>>;
        SliceVec slicesVec = slices.empty() ? SliceVec() : b.CreateVector(slices);
        QuoteVec quotesVec = quotes.empty() ? QuoteVec() : b.CreateVector(quotes);
        auto discId = b.CreateString("discount");
        auto divId = b.CreateString("div");
        quantra::BlackVolSpecBuilder vb(b);
        vb.add_base(base);
        vb.add_spot(in.spot);
        if (in.useCurves) {
            vb.add_discount_curve_id(discId);
            vb.add_dividend_curve_id(divId);
        } else {
            vb.add_use_flat_discount_rate(true);
            vb.add_flat_discount_rate(in.riskFree);
            if (!in.noDividend) {
                vb.add_use_flat_dividend_rate(true);
                vb.add_flat_dividend_rate(in.dividend);
            }
        }
        if (!slices.empty()) vb.add_ssvi_slices(slicesVec);
        if (!quotes.empty()) vb.add_option_quotes(quotesVec);
        if (in.withFit) vb.add_ssvi_fit(fit);
        auto spec = vb.Finish();

        auto id = b.CreateString(in.id);
        quantra::VolSurfaceSpecBuilder vsb(b);
        vsb.add_id(id);
        vsb.add_payload_type(quantra::VolPayload_BlackVolSpec);
        vsb.add_payload(spec.Union());
        return vsb.Finish();
    }

    // Pricing with the surface, the discount / dividend curves, the spot quote, the Black-Scholes
    // model and one underlying: enough for /calibrate-equity-vol and /price-equity-option.
    flatbuffers::Offset<quantra::Pricing> buildPricingFor(
        flatbuffers::grpc::MessageBuilder& b, const VolIn& in) {
        auto curves = b.CreateVector(
            std::vector<flatbuffers::Offset<quantra::TermStructure>>{
                buildCurve(b, "discount"), buildCurve(b, "div")});
        auto quoteId = b.CreateString("EQ_SPOT");
        quantra::QuoteSpecBuilder qb(b);
        qb.add_id(quoteId);
        qb.add_kind(quantra::QuoteKind_Price);
        qb.add_value(in.spot);
        qb.add_quote_type(quantra::QuoteType_Curve);
        auto quotes =
            b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{qb.Finish()});
        auto vols = b.CreateVector(
            std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{buildVol(b, in)});

        quantra::EquityVanillaModelSpecBuilder emsb(b);
        emsb.add_model_type(quantra::enums::EquityModelType_BlackScholesAnalytic);
        auto eqModel = emsb.Finish();
        auto modelId = b.CreateString("eq_model");
        quantra::ModelSpecBuilder msb(b);
        msb.add_id(modelId);
        msb.add_payload_type(quantra::ModelPayload_EquityVanillaModelSpec);
        msb.add_payload(eqModel.Union());
        auto models =
            b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{msb.Finish()});

        auto undId = b.CreateString("EQ1");
        auto ccy = b.CreateString("USD");
        auto divCurve = b.CreateString("div");
        quantra::EquityUnderlyingSpecBuilder usb(b);
        usb.add_id(undId);
        usb.add_currency(ccy);
        usb.add_spot_quote_id(quoteId);
        usb.add_dividend_yield_curve_id(divCurve);
        auto unds = b.CreateVector(
            std::vector<flatbuffers::Offset<quantra::EquityUnderlyingSpec>>{usb.Finish()});
        return buildPricing(
            b, b.CreateString("2025-01-15"), 0, quotes, buildIndicesVector(b), 0, curves, 0, 0,
            vols, models, unds);
    }

    // Runs /calibrate-equity-vol; the response lives in the returned builder.
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> calibrate(
        const VolIn& in, const std::string& volId = "") {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildPricingFor(b, in);
        auto volIdStr = b.CreateString(volId.empty() ? in.id : volId);
        quantra::CalibrateEquityVolRequestBuilder rb(b);
        rb.add_pricing(pricing);
        rb.add_vol_id(volIdStr);
        b.Finish(rb.Finish());
        CalibrateEquityVolEndpoint handler;
        auto out = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        out->Finish(handler.request(
            out, flatbuffers::GetRoot<quantra::CalibrateEquityVolRequest>(b.GetBufferPointer())));
        return out;
    }

    // NPV of a European call (strike, expiry in days) on the underlying "EQ1" priced with `in`.
    double callNpv(const VolIn& in, double strike, int days) {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildPricingFor(b, in);
        quantra::EquityPlainVanillaPayoffBuilder pvb(b);
        pvb.add_option_type(quantra::enums::EquityOptionType_Call);
        pvb.add_strike(strike);
        auto payoff = pvb.Finish();
        auto expiryStr = b.CreateString(isoAfter(days));
        quantra::EquityEuropeanExerciseBuilder eeb(b);
        eeb.add_expiry_date(expiryStr);
        auto exercise = eeb.Finish();
        auto tradeId = b.CreateString("CALL");
        auto trUnd = b.CreateString("EQ1");
        quantra::EquityOptionBuilder eob(b);
        eob.add_trade_id(tradeId);
        eob.add_underlying_id(trUnd);
        eob.add_quantity(1.0);
        eob.add_settlement(quantra::enums::EquitySettlementType_Physical);
        eob.add_payoff_type(quantra::EquityPayoff_EquityPlainVanillaPayoff);
        eob.add_payoff(payoff.Union());
        eob.add_exercise_type(quantra::EquityExercise_EquityEuropeanExercise);
        eob.add_exercise(exercise.Union());
        auto option = eob.Finish();
        auto pDisc = b.CreateString("discount");
        auto pVol = b.CreateString(in.id);
        auto pModel = b.CreateString("eq_model");
        quantra::PriceEquityOptionBuilder peb(b);
        peb.add_option(option);
        peb.add_discounting_curve(pDisc);
        peb.add_volatility(pVol);
        peb.add_model(pModel);
        auto po = peb.Finish();
        auto optionsVec =
            b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceEquityOption>>{po});
        quantra::PriceEquityOptionRequestBuilder reqb(b);
        reqb.add_pricing(pricing);
        reqb.add_options(optionsVec);
        b.Finish(reqb.Finish());
        EquityOptionPricingRequest handler;
        auto out = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        out->Finish(handler.request(
            out, flatbuffers::GetRoot<quantra::PriceEquityOptionRequest>(b.GetBufferPointer())));
        auto resp =
            flatbuffers::GetRoot<quantra::PriceEquityOptionResponse>(out->GetBufferPointer());
        return resp->options()->Get(0)->npv();
    }

    // The registry built from the pricing of `in` (SsviParams / SsviCalibrate surface "in.id").
    PricingRegistry registryFor(const VolIn& in) {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildPricingFor(b, in);
        b.Finish(pricing);
        return PricingRegistryBuilder().build(
            flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer()));
    }

    // Building the registry throws QuantraInvalidArgument (HTTP 400).
    void expectInvalid(const VolIn& in) { EXPECT_THROW(registryFor(in), QuantraInvalidArgument); }

    VolIn paramsFrom(const std::vector<SliceIn>& slices) {
        VolIn in;
        in.shape = quantra::enums::VolSurfaceShape_SsviParams;
        in.slices = slices;
        return in;
    }
};

} // namespace

TEST_F(CalibrateEquityVolTest, RecoversTruthAndReportsForwardsOnEverySlice) {
    VolIn in;
    in.quotes = truthQuotes(kGoodSet, in.spot, in.riskFree, in.dividend);
    in.withFit = true;
    auto out = calibrate(in);
    auto resp = flatbuffers::GetRoot<quantra::CalibrateEquityVolResponse>(out->GetBufferPointer());
    ASSERT_NE(resp, nullptr);
    EXPECT_EQ(resp->vol_id()->str(), "eq_vol");
    ASSERT_EQ(resp->slices()->size(), kGoodSet.size());
    for (flatbuffers::uoffset_t i = 0; i < resp->slices()->size(); ++i) {
        const auto* s = resp->slices()->Get(i);
        EXPECT_EQ(s->expiry_date()->str(), isoAfter(kGoodSet[i].days));
        EXPECT_NEAR(s->theta().value(), kGoodSet[i].theta, 1e-6);
        EXPECT_NEAR(s->rho().value(), kGoodSet[i].rho, 1e-4);
        EXPECT_NEAR(s->psi().value(), kGoodSet[i].psi, 1e-4);
        ASSERT_TRUE(s->forward().has_value()) << "slice " << i << " has no forward";
        EXPECT_NEAR(
            s->forward().value(),
            in.spot * std::exp((in.riskFree - in.dividend) * yearFraction(kGoodSet[i].days)), 1e-8);
    }
    const auto* d = resp->diagnostics();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->slices()->size(), kGoodSet.size());
    EXPECT_EQ(d->quotes()->size(), in.quotes.size());
    EXPECT_LT(d->overall_max_abs_error(), 1e-5);
    EXPECT_EQ(d->violations()->size(), 0u);
    EXPECT_GE(d->scan_min_g(), 0.0);
    EXPECT_GE(d->scan_min_w_t(), 0.0);
}

TEST_F(CalibrateEquityVolTest, ResponseSlicesRoundTripToIdenticalSsviParamsSurface) {
    VolIn cal;
    cal.useCurves = true;
    cal.quotes = truthQuotes(kGoodSet, cal.spot, 0.03, 0.03);
    cal.withFit = true;
    auto out = calibrate(cal);
    auto resp = flatbuffers::GetRoot<quantra::CalibrateEquityVolResponse>(out->GetBufferPointer());
    ASSERT_NE(resp, nullptr);

    VolIn params;
    params.useCurves = true;
    params.shape = quantra::enums::VolSurfaceShape_SsviParams;
    for (flatbuffers::uoffset_t i = 0; i < resp->slices()->size(); ++i) {
        const auto* s = resp->slices()->Get(i);
        params.slices.push_back(
            SliceIn{
                kGoodSet[i].days, s->theta().value(), s->rho().value(), s->psi().value(),
                s->forward().value()});
    }
    for (int days : {60, 91, 150, 300, 700}) {
        const double fromCalibration = callNpv(cal, 100.0, days);
        const double fromParams = callNpv(params, 100.0, days);
        EXPECT_NEAR(fromCalibration, fromParams, 1e-12) << "expiry " << days << "d";
    }
}

TEST_F(CalibrateEquityVolTest, EuropeanCallOnSsviParamsMatchesAnalyticEngineOnDirectSurface) {
    VolIn in = paramsFrom(kGoodSet);
    in.useCurves = true;
    const double strike = 95.0;
    const int days = 300;

    const auto ref = evaluationDate_;
    std::vector<QuantLib::Date> expiries;
    std::vector<SsviSliceParams> params;
    for (const SliceIn& s : kGoodSet) {
        expiries.push_back(ref + s.days);
        params.push_back(SsviSliceParams{s.theta, s.rho, s.psi});
    }
    auto vol = std::make_shared<SsviBlackVol>(
        ref, QuantLib::TARGET(), QuantLib::ModifiedFollowing, QuantLib::Actual365Fixed(), expiries,
        params, in.spot, discountHandle_, discountHandle_);
    auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, strike);
    auto exercise = std::make_shared<QuantLib::EuropeanExercise>(ref + days);
    QuantLib::VanillaOption option(payoff, exercise);
    auto spot = QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(in.spot));
    auto process = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        spot, discountHandle_, discountHandle_,
        QuantLib::Handle<QuantLib::BlackVolTermStructure>(vol));
    option.setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));

    EXPECT_NEAR(callNpv(in, strike, days), option.NPV(), 1e-12);
}

TEST_F(CalibrateEquityVolTest, SsviSurfacesAnswerBlackVolQueriesThroughTheRegistryHandle) {
    VolIn params = paramsFrom(kGoodSet);
    VolIn cal;
    cal.quotes = truthQuotes(kGoodSet, cal.spot, cal.riskFree, cal.dividend);
    cal.withFit = true;
    for (const VolIn* in : {&params, &cal}) {
        PricingRegistry reg = registryFor(*in);
        const BlackVolEntry& entry = reg.volatility.blackVols.at(in->id);
        for (int days : {45, 91, 250, 600}) {
            for (double strike : {70.0, 100.0, 130.0}) {
                const double v = entry.handle->blackVol(evaluationDate_ + days, strike);
                EXPECT_TRUE(std::isfinite(v));
                EXPECT_GT(v, 0.0);
            }
        }
    }
}

TEST_F(CalibrateEquityVolTest, SecondIdenticalRequestHitsTheCalibrateCache) {
    SsviCalibrateCache::setEnabledOverrideForTesting(true);
    SsviCalibrateCache::instance().clear();
    VolIn in;
    in.quotes = truthQuotes(kGoodSet, in.spot, in.riskFree, in.dividend);
    in.withFit = true;
    calibrate(in);
    EXPECT_EQ(SsviCalibrateCache::instance().hits(), 0u);
    EXPECT_EQ(SsviCalibrateCache::instance().size(), 1u);
    calibrate(in);
    EXPECT_EQ(SsviCalibrateCache::instance().hits(), 1u);
    in.quotes[0].vol = *in.quotes[0].vol + 0.001; // different content, different key
    calibrate(in);
    EXPECT_EQ(SsviCalibrateCache::instance().size(), 2u);
}

TEST_F(CalibrateEquityVolTest, CalibrateEndpointRejectsMissingAndWrongShapeSurfaces) {
    VolIn good;
    good.quotes = truthQuotes(kGoodSet, good.spot, good.riskFree, good.dividend);
    good.withFit = true;
    EXPECT_THROW(calibrate(good, "no_such_vol"), QuantraNotFound);
    VolIn params = paramsFrom(kGoodSet);
    EXPECT_THROW(calibrate(params), QuantraInvalidArgument);
}

TEST_F(CalibrateEquityVolTest, CalibrateRejectsMalformedQuotes) {
    VolIn base;
    base.quotes = truthQuotes(kGoodSet, base.spot, base.riskFree, base.dividend);
    base.withFit = true;

    VolIn both = base;
    both.quotes[0].price = 5.0; // price and implied_vol
    expectInvalid(both);

    VolIn bidOnly = base;
    bidOnly.quotes[0].vol.reset();
    bidOnly.quotes[0].bid = 4.0;
    expectInvalid(bidOnly);

    VolIn none = base;
    none.quotes[0].vol.reset();
    expectInvalid(none);

    VolIn bidAskMissing = base; // weighting=BidAsk needs bid and ask on every quote
    bidAskMissing.weighting = quantra::enums::SsviQuoteWeighting_BidAsk;
    expectInvalid(bidAskMissing);

    VolIn noQuotes = base;
    noQuotes.quotes.clear();
    expectInvalid(noQuotes);

    VolIn withSlices = base;
    withSlices.slices = kGoodSet;
    expectInvalid(withSlices);
}

TEST_F(CalibrateEquityVolTest, SsviParamsValidationReturns400) {
    auto good = paramsFrom(kGoodSet);
    EXPECT_NO_THROW(registryFor(good));

    VolIn empty = good;
    empty.slices.clear();
    expectInvalid(empty);

    VolIn unordered = good;
    std::swap(unordered.slices[1], unordered.slices[2]);
    expectInvalid(unordered);

    VolIn pastExpiry = good;
    pastExpiry.slices[0].days = -5;
    expectInvalid(pastExpiry);

    VolIn badRho = good;
    badRho.slices[1].rho = 1.5;
    expectInvalid(badRho);

    VolIn badTheta = good;
    badTheta.slices[1].theta = -0.01;
    expectInvalid(badTheta);

    VolIn nonFinite = good;
    nonFinite.slices[2].psi = std::numeric_limits<double>::quiet_NaN();
    expectInvalid(nonFinite);

    VolIn butterfly = good; // psi (1 + |rho|) > 4
    butterfly.slices[0].psi = 5.0;
    expectInvalid(butterfly);

    VolIn calendar = good; // total variance decreasing in maturity
    calendar.slices[2].theta = calendar.slices[1].theta * 0.9;
    expectInvalid(calendar);

    VolIn mixed = good; // forward on some slices only
    mixed.slices[0].forward = 101.0;
    expectInvalid(mixed);

    VolIn badForward = good;
    for (SliceIn& s : badForward.slices)
        s.forward = -1.0;
    expectInvalid(badForward);

    VolIn withQuotes = good;
    withQuotes.quotes = truthQuotes(kGoodSet, 100.0, 0.04, 0.015);
    expectInvalid(withQuotes);

    VolIn withFit = good;
    withFit.withFit = true;
    expectInvalid(withFit);
}

TEST_F(CalibrateEquityVolTest, ViolationMessageListsEveryOffendingCondition) {
    VolIn bad = paramsFrom(kGoodSet);
    bad.slices[0].psi = 5.0;
    bad.slices[2].theta = bad.slices[1].theta * 0.9;
    try {
        registryFor(bad);
        FAIL() << "expected QuantraInvalidArgument";
    } catch (const QuantraInvalidArgument& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("slice 0"), std::string::npos) << msg;
        EXPECT_NE(msg.find("slice 2"), std::string::npos) << msg;
    }
}

TEST_F(CalibrateEquityVolTest, NonSsviShapeRejectsSsviFields) {
    VolIn in = paramsFrom(kGoodSet);
    in.shape = quantra::enums::VolSurfaceShape_Constant;
    expectInvalid(in);
}

// Structured-note greeks bump the vol with shiftedBlackVol(entry, volBump) (default volBump 0.01,
// up-bump only; the equity option evaluator uses analytic vega). An inverted equity term
// structure -- ATM vols 0.35 / 0.30 / 0.26 / 0.24 at 0.25 / 0.5 / 1 / 2y with a skew -- calibrates
// onto the calendar condition (i), which is where a bump of the ATM vols is most likely to break
// the surface.
TEST_F(CalibrateEquityVolTest, InvertedTermStructureSurvivesTheGreeksVolBumpBothWays) {
    VolIn in;
    const std::vector<std::pair<int, double>> atm{
        {91, 0.35}, {182, 0.30}, {365, 0.26}, {730, 0.24}};
    for (const auto& [days, atmVol] : atm) {
        const double T = yearFraction(days);
        const double F = in.spot * std::exp((in.riskFree - in.dividend) * T);
        for (double m : {0.7, 0.8, 0.9, 1.0, 1.1, 1.2, 1.3}) {
            const double k = std::log(m);
            QuoteIn q;
            q.days = days;
            q.strike = F * m;
            q.vol = atmVol - 0.18 * k + 0.12 * k * k; // equity skew with mild convexity
            in.quotes.push_back(q);
        }
    }
    in.withFit = true;
    PricingRegistry reg = registryFor(in);
    const BlackVolEntry& entry = reg.volatility.blackVols.at(in.id);
    ASSERT_TRUE(entry.ssvi != nullptr);
    for (std::size_t i = 0; i < entry.ssvi->fit->slices.size(); ++i) {
        const auto& s = entry.ssvi->fit->slices[i];
        std::cout << "[inverted] slice " << i << " theta=" << entry.ssvi->params[i].theta
                  << " rho=" << entry.ssvi->params[i].rho << " psi=" << entry.ssvi->params[i].psi
                  << " calendar margins theta/psi/a=" << s.marginCalendarTheta << "/"
                  << s.marginCalendarPsi << "/" << s.marginCalendarA
                  << " clamped=" << s.anchorClamped << '\n';
    }
    for (double bump : {0.01, -0.01, 0.05, -0.05}) {
        EXPECT_NO_THROW(shiftedBlackVol(entry, bump)) << "vol bump " << bump;
    }
    const double base = entry.handle->blackVol(evaluationDate_ + 365, 100.0);
    // The shift moves the ATM vol of every slice, so the vol at a fixed strike moves by about it.
    const double up = shiftedBlackVol(entry, 0.01)->blackVol(evaluationDate_ + 365, 100.0);
    EXPECT_GT(up, base + 0.005);
    EXPECT_LT(up, base + 0.015);
}

// Worst case for the bump: inverted ATM vols with psi_i / theta_i constant, i.e. exactly on the
// calendar condition (i) of ssvi.h (phi = psi / theta non-increasing). Both bump signs stay
// arbitrage free (condition (ii) of the same proposition takes over when (i) is lost).
TEST_F(CalibrateEquityVolTest, InvertedTermStructureOnTheCalendarBoundVolBump) {
    const std::vector<std::pair<int, double>> atm{
        {91, 0.35}, {182, 0.30}, {365, 0.26}, {730, 0.24}};
    std::vector<SliceIn> slices;
    for (const auto& [days, atmVol] : atm) {
        const double theta = atmVol * atmVol * yearFraction(days);
        slices.push_back(SliceIn{days, theta, -0.6, 3.0 * theta, std::nullopt});
    }
    VolIn in = paramsFrom(slices);
    PricingRegistry reg = registryFor(in);
    const BlackVolEntry& entry = reg.volatility.blackVols.at(in.id);
    for (double bump : {0.01, -0.01, 0.05, -0.05}) {
        EXPECT_NO_THROW(shiftedBlackVol(entry, bump)) << "vol bump " << bump;
    }
}

namespace {

struct Fitted {
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> buffer;
    const quantra::CalibrateEquityVolResponse* resp;
    const quantra::EquityVolQuoteDiagnostics* quote(std::size_t i) const {
        return resp->diagnostics()->quotes()->Get(static_cast<flatbuffers::uoffset_t>(i));
    }
    std::string reason(std::size_t i) const { return quote(i)->exclusion_reason()->str(); }
};

} // namespace

// A bad quote is excluded and reported; the fit continues from the rest.
TEST_F(CalibrateEquityVolTest, CallBelowIntrinsicIsExcludedAndTheFitStillRecoversTruth) {
    VolIn in;
    in.quotes = parityQuotes(kGoodSet, in.spot, in.riskFree, in.dividend);
    in.withFit = true;
    in.quotes[0].price = 0.5; // 91d call at 80% of the forward: far below intrinsic
    Fitted f{calibrate(in), nullptr};
    f.resp =
        flatbuffers::GetRoot<quantra::CalibrateEquityVolResponse>(f.buffer->GetBufferPointer());
    EXPECT_FALSE(f.quote(0)->used());
    EXPECT_EQ(f.reason(0), "price_outside_bounds");
    EXPECT_FALSE(f.quote(0)->market_vol().has_value());
    EXPECT_EQ(f.reason(1), "parity_outlier"); // its pair partner goes with it
    for (flatbuffers::uoffset_t i = 0; i < f.resp->slices()->size(); ++i) {
        EXPECT_NEAR(f.resp->slices()->Get(i)->theta().value(), kGoodSet[i].theta, 1e-6);
        EXPECT_NEAR(f.resp->slices()->Get(i)->psi().value(), kGoodSet[i].psi, 1e-5);
    }
    EXPECT_EQ(f.resp->diagnostics()->slices()->Get(0)->parity_pairs_rejected(), 1u);
}

TEST_F(CalibrateEquityVolTest, PairWithCallShiftedTwoPercentIsAParityOutlier) {
    VolIn in;
    in.quotes = parityQuotes(kGoodSet, in.spot, in.riskFree, in.dividend);
    in.withFit = true;
    const std::size_t call = 10 + 6; // 182d call at 110% of the forward
    *in.quotes[call].price *= 1.02;
    Fitted f{calibrate(in), nullptr};
    f.resp =
        flatbuffers::GetRoot<quantra::CalibrateEquityVolResponse>(f.buffer->GetBufferPointer());
    EXPECT_EQ(f.reason(call), "parity_outlier");
    EXPECT_EQ(f.reason(call + 1), "parity_outlier");
    EXPECT_FALSE(f.quote(call)->used());
    const auto* s = f.resp->slices()->Get(1);
    const double trueForward = in.spot * std::exp((in.riskFree - in.dividend) * yearFraction(182));
    EXPECT_NEAR(s->forward().value() / trueForward, 1.0, 1e-10);
    EXPECT_NEAR(s->theta().value(), kGoodSet[1].theta, 1e-6);
    const auto* d = f.resp->diagnostics()->slices()->Get(1);
    EXPECT_EQ(d->parity_pairs_used(), 4u);
    EXPECT_EQ(d->parity_pairs_rejected(), 1u);
}

TEST_F(CalibrateEquityVolTest, CrossedBidAskIsExcludedUnderBidAskWeighting) {
    VolIn in;
    in.quotes = parityQuotes(kGoodSet, in.spot, in.riskFree, in.dividend, true);
    in.withFit = true;
    in.weighting = quantra::enums::SsviQuoteWeighting_BidAsk;
    const std::size_t call = 2 * 10 + 8; // 365d call at 120%: the out-of-the-money quote
    in.quotes[call].ask = *in.quotes[call].bid - 0.01;
    Fitted f{calibrate(in), nullptr};
    f.resp =
        flatbuffers::GetRoot<quantra::CalibrateEquityVolResponse>(f.buffer->GetBufferPointer());
    EXPECT_FALSE(f.quote(call)->used());
    EXPECT_EQ(f.reason(call), "ask_not_above_bid");
    EXPECT_TRUE(f.quote(call - 2)->used());
}

TEST_F(CalibrateEquityVolTest, SliceWithTwoUsedQuotesIsRejectedNamingExpiryAndCounts) {
    VolIn in;
    in.quotes = truthQuotes(kGoodSet, in.spot, in.riskFree, in.dividend);
    in.withFit = true;
    in.maxAbsLogMoneyness = 0.1; // keeps 100% and 110% of the forward: 2 of 9 per slice
    try {
        registryFor(in);
        FAIL() << "expected QuantraInvalidArgument";
    } catch (const QuantraInvalidArgument& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find(isoAfter(91)), std::string::npos) << msg;
        EXPECT_NE(msg.find("has 2 used quotes"), std::string::npos) << msg;
        EXPECT_NE(msg.find("max_abs_log_moneyness=7"), std::string::npos) << msg;
    }
}

TEST_F(CalibrateEquityVolTest, ExactDataRejectsNothingAndRecoversTruth) {
    VolIn in;
    in.quotes = parityQuotes(kGoodSet, in.spot, in.riskFree, in.dividend);
    in.withFit = true;
    Fitted f{calibrate(in), nullptr};
    f.resp =
        flatbuffers::GetRoot<quantra::CalibrateEquityVolResponse>(f.buffer->GetBufferPointer());
    for (flatbuffers::uoffset_t i = 0; i < f.resp->slices()->size(); ++i) {
        const auto* d = f.resp->diagnostics()->slices()->Get(i);
        EXPECT_EQ(d->parity_pairs_rejected(), 0u);
        EXPECT_EQ(d->parity_pairs_used(), 5u);
        EXPECT_NEAR(f.resp->slices()->Get(i)->theta().value(), kGoodSet[i].theta, 1e-9);
        EXPECT_NEAR(f.resp->slices()->Get(i)->psi().value(), kGoodSet[i].psi, 1e-9);
    }
    for (std::size_t i = 0; i < in.quotes.size(); ++i) {
        EXPECT_NE(f.reason(i), "parity_outlier");
        EXPECT_NE(f.reason(i), "price_outside_bounds");
    }
}

TEST_F(CalibrateEquityVolTest, CalibrateWithoutDividendCurveUsesParityForwards) {
    VolIn in;
    const double r = in.riskFree;
    const double q = in.dividend;
    for (const SliceIn& s : kGoodSet) {
        const double T = yearFraction(s.days);
        const double F = in.spot * std::exp((r - q) * T);
        const ssvi::Node n{s.theta, s.psi, s.rho * s.psi, 0, 0, 0};
        const double D = std::exp(-r * T);
        for (double m : {0.8, 0.9, 1.0, 1.1, 1.2}) {
            const double sd = std::sqrt(ssvi::w(n, std::log(m)));
            const double K = F * m;
            QuoteIn call, put;
            call.days = put.days = s.days;
            call.strike = put.strike = K;
            put.put = true;
            call.price = D * QuantLib::blackFormula(QuantLib::Option::Call, K, F, sd);
            put.price = D * QuantLib::blackFormula(QuantLib::Option::Put, K, F, sd);
            in.quotes.push_back(call);
            in.quotes.push_back(put);
        }
    }
    in.withFit = true;
    in.noDividend = true;
    auto out = calibrate(in);
    auto resp = flatbuffers::GetRoot<quantra::CalibrateEquityVolResponse>(out->GetBufferPointer());
    ASSERT_EQ(resp->slices()->size(), kGoodSet.size());
    for (flatbuffers::uoffset_t i = 0; i < resp->slices()->size(); ++i) {
        const double T = yearFraction(kGoodSet[i].days);
        EXPECT_NEAR(resp->slices()->Get(i)->theta().value(), kGoodSet[i].theta, 1e-6);
        EXPECT_NEAR(
            resp->slices()->Get(i)->forward().value(), in.spot * std::exp((r - q) * T), 1e-6);
        const auto* d = resp->diagnostics()->slices()->Get(i);
        EXPECT_FALSE(d->curve_forward().has_value());
        EXPECT_NEAR(d->implied_dividend_yield(), q, 1e-6);
    }
}

TEST_F(CalibrateEquityVolTest, CalibrateWithoutDividendCurveRejectsExpiryWithoutParityPairs) {
    VolIn in;
    in.noDividend = true;
    in.quotes = truthQuotes(kGoodSet, in.spot, in.riskFree, in.dividend); // calls only
    in.withFit = true;
    try {
        registryFor(in);
        FAIL() << "expected QuantraInvalidArgument";
    } catch (const QuantraInvalidArgument& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("dividend curve"), std::string::npos) << msg;
        EXPECT_NE(msg.find(isoAfter(91).substr(0, 4)), std::string::npos) << msg;
    }
}

}} // namespace quantra::testing
