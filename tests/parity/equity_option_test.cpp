// Equity Option parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, EquityOption_EuropeanVanilla_NPVMatches) {
    const double spot = 100.0;
    const double strike = 100.0;
    const double vol = 0.20;
    const auto expiry = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);

    auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, strike);
    auto exercise = std::make_shared<QuantLib::EuropeanExercise>(expiry);
    auto qlOption = std::make_shared<QuantLib::VanillaOption>(payoff, exercise);
    auto qlSpot = QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot));
    auto qlVol = QuantLib::Handle<QuantLib::BlackVolTermStructure>(
        std::make_shared<QuantLib::BlackConstantVol>(
            evaluationDate_, QuantLib::TARGET(), vol, QuantLib::Actual365Fixed()));
    auto process = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        qlSpot, discountHandle_, discountHandle_, qlVol);
    qlOption->setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
    const double qlNpv = qlOption->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto asof = b.CreateString("2025-01-15");

    auto curveDiscount = buildCurve(b, "discount");
    auto curveDividend = buildCurve(b, "div");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        curveDiscount, curveDividend});

    auto quoteId = b.CreateString("EQ_SPOT");
    quantra::QuoteSpecBuilder qb(b);
    qb.add_id(quoteId);
    qb.add_kind(quantra::QuoteKind_Price);
    qb.add_value(spot);
    qb.add_quote_type(quantra::QuoteType_Curve);
    auto quote = qb.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{quote});

    auto volRef = b.CreateString("2025-01-15");
    quantra::BlackVolBaseSpecBuilder bvbb(b);
    bvbb.add_reference_date(volRef);
    bvbb.add_calendar(quantra::enums::Calendar_TARGET);
    bvbb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    bvbb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    bvbb.add_shape(quantra::enums::VolSurfaceShape_Constant);
    bvbb.add_constant_vol(vol);
    auto blackBase = bvbb.Finish();
    quantra::BlackVolSpecBuilder bvsb(b);
    bvsb.add_base(blackBase);
    auto blackSpec = bvsb.Finish();
    auto volId = b.CreateString("eq_vol");
    quantra::VolSurfaceSpecBuilder vssb(b);
    vssb.add_id(volId);
    vssb.add_payload_type(quantra::VolPayload_BlackVolSpec);
    vssb.add_payload(blackSpec.Union());
    auto volSurface = vssb.Finish();
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    quantra::EquityVanillaModelSpecBuilder emsb(b);
    emsb.add_model_type(quantra::enums::EquityModelType_BlackScholesAnalytic);
    auto eqModelSpec = emsb.Finish();
    auto modelId = b.CreateString("eq_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(modelId);
    msb.add_payload_type(quantra::ModelPayload_EquityVanillaModelSpec);
    msb.add_payload(eqModelSpec.Union());
    auto model = msb.Finish();
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto undId = b.CreateString("EQ1");
    auto undCcy = b.CreateString("USD");
    auto divCurveId = b.CreateString("div");
    quantra::EquityUnderlyingSpecBuilder usb(b);
    usb.add_id(undId);
    usb.add_currency(undCcy);
    usb.add_spot_quote_id(quoteId);
    usb.add_dividend_yield_curve_id(divCurveId);
    auto und = usb.Finish();
    auto underlyings =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::EquityUnderlyingSpec>>{und});
    auto indices = buildIndicesVector(b);

    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, vols, models, underlyings);

    quantra::EquityPlainVanillaPayoffBuilder pvb(b);
    pvb.add_option_type(quantra::enums::EquityOptionType_Call);
    pvb.add_strike(strike);
    auto payoffFb = pvb.Finish();

    auto expiryStr = b.CreateString("2026-01-15");
    quantra::EquityEuropeanExerciseBuilder eeb(b);
    eeb.add_expiry_date(expiryStr);
    auto exFb = eeb.Finish();

    auto tradeId = b.CreateString("EQ_CALL_1Y");
    quantra::EquityOptionBuilder eob(b);
    eob.add_trade_id(tradeId);
    eob.add_underlying_id(undId);
    eob.add_quantity(1.0);
    eob.add_settlement(quantra::enums::EquitySettlementType_Physical);
    eob.add_payoff_type(quantra::EquityPayoff_EquityPlainVanillaPayoff);
    eob.add_payoff(payoffFb.Union());
    eob.add_exercise_type(quantra::EquityExercise_EquityEuropeanExercise);
    eob.add_exercise(exFb.Union());
    auto option = eob.Finish();

    auto discId = b.CreateString("discount");
    quantra::PriceEquityOptionBuilder peob(b);
    peob.add_option(option);
    peob.add_discounting_curve(discId);
    peob.add_volatility(volId);
    peob.add_model(modelId);
    auto po = peob.Finish();
    auto options = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceEquityOption>>{po});

    quantra::PriceEquityOptionRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_options(options);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceEquityOptionRequest>(b.GetBufferPointer());
    EquityOptionPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::PriceEquityOptionResponse>(outBuilder->GetBufferPointer());

    ASSERT_NE(resp->options(), nullptr);
    ASSERT_EQ(resp->options()->size(), 1u);
    const auto* r = resp->options()->Get(0);
    ASSERT_NE(r, nullptr);
    EXPECT_NEAR(r->npv(), qlNpv, 1.0e-8);
}

TEST_F(QuantraComparisonTest, EquityOption_BlackTermShapeMissingGridRejected) {
    flatbuffers::grpc::MessageBuilder b;
    auto asof = b.CreateString("2025-01-15");

    auto curveDiscount = buildCurve(b, "discount");
    auto curveDividend = buildCurve(b, "div");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        curveDiscount, curveDividend});

    auto quoteId = b.CreateString("EQ_SPOT");
    quantra::QuoteSpecBuilder qb(b);
    qb.add_id(quoteId);
    qb.add_kind(quantra::QuoteKind_Price);
    qb.add_value(100.0);
    qb.add_quote_type(quantra::QuoteType_Curve);
    auto quote = qb.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{quote});

    auto volRef = b.CreateString("2025-01-15");
    quantra::BlackVolBaseSpecBuilder bvbb(b);
    bvbb.add_reference_date(volRef);
    bvbb.add_calendar(quantra::enums::Calendar_TARGET);
    bvbb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    bvbb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    bvbb.add_shape(quantra::enums::VolSurfaceShape_AtmMatrix2D);
    bvbb.add_constant_vol(0.20);
    auto blackBase = bvbb.Finish();
    quantra::BlackVolSpecBuilder bvsb(b);
    bvsb.add_base(blackBase);
    auto blackSpec = bvsb.Finish();
    auto volId = b.CreateString("eq_surface_like_vol");
    quantra::VolSurfaceSpecBuilder vssb(b);
    vssb.add_id(volId);
    vssb.add_payload_type(quantra::VolPayload_BlackVolSpec);
    vssb.add_payload(blackSpec.Union());
    auto volSurface = vssb.Finish();
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    quantra::EquityVanillaModelSpecBuilder emsb(b);
    emsb.add_model_type(quantra::enums::EquityModelType_BlackScholesAnalytic);
    auto eqModelSpec = emsb.Finish();
    auto modelId = b.CreateString("eq_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(modelId);
    msb.add_payload_type(quantra::ModelPayload_EquityVanillaModelSpec);
    msb.add_payload(eqModelSpec.Union());
    auto model = msb.Finish();
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto undId = b.CreateString("EQ1");
    auto undCcy = b.CreateString("USD");
    auto divCurveId = b.CreateString("div");
    quantra::EquityUnderlyingSpecBuilder usb(b);
    usb.add_id(undId);
    usb.add_currency(undCcy);
    usb.add_spot_quote_id(quoteId);
    usb.add_dividend_yield_curve_id(divCurveId);
    auto und = usb.Finish();
    auto underlyings =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::EquityUnderlyingSpec>>{und});
    auto indices = buildIndicesVector(b);

    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, vols, models, underlyings);

    quantra::EquityPlainVanillaPayoffBuilder pvb(b);
    pvb.add_option_type(quantra::enums::EquityOptionType_Call);
    pvb.add_strike(100.0);
    auto payoffFb = pvb.Finish();

    auto expiryStr = b.CreateString("2026-01-15");
    quantra::EquityEuropeanExerciseBuilder eeb(b);
    eeb.add_expiry_date(expiryStr);
    auto exFb = eeb.Finish();

    auto tradeId = b.CreateString("EQ_CALL_SURFACE_SHAPE");
    quantra::EquityOptionBuilder eob(b);
    eob.add_trade_id(tradeId);
    eob.add_underlying_id(undId);
    eob.add_quantity(1.0);
    eob.add_settlement(quantra::enums::EquitySettlementType_Physical);
    eob.add_payoff_type(quantra::EquityPayoff_EquityPlainVanillaPayoff);
    eob.add_payoff(payoffFb.Union());
    eob.add_exercise_type(quantra::EquityExercise_EquityEuropeanExercise);
    eob.add_exercise(exFb.Union());
    auto option = eob.Finish();

    auto discId = b.CreateString("discount");
    quantra::PriceEquityOptionBuilder peob(b);
    peob.add_option(option);
    peob.add_discounting_curve(discId);
    peob.add_volatility(volId);
    peob.add_model(modelId);
    auto po = peob.Finish();
    auto options = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceEquityOption>>{po});

    quantra::PriceEquityOptionRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_options(options);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceEquityOptionRequest>(b.GetBufferPointer());
    EquityOptionPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(handler.request(outBuilder, req), QuantraError);
}

}} // namespace quantra::testing
