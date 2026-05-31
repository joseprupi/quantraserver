// Year-on-Year Inflation Swap parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, PriceYearOnYearInflationSwap_MatchesQuantLib) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    auto idxId = b.CreateString("EUHICP_YY");
    auto idxFamily = b.CreateString("EU HICP YoY");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(0.0180);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(0.0190);
    auto fixNov = fixNovBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{fixOct, fixNov});

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_YY");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);

    quantra::YearOnYearInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    h1Builder.add_nominal_curve_id(discCurveId);
    auto h1 = h1Builder.Finish();

    quantra::YearOnYearInflationSwapHelperBuilder h2Builder(b);
    h2Builder.add_quote_value(0.0210);
    h2Builder.add_swap_observation_lag(observationLag);
    h2Builder.add_tenor(p2y);
    h2Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h2Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h2Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h2Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    h2Builder.add_nominal_curve_id(discCurveId);
    auto h2 = h2Builder.Finish();

    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
    pw2Builder.add_point(h2.Union());
    auto pwh2 = pw2Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{
        pwh1, pwh2
    });

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto effective = b.CreateString("2025-01-15");
    auto termination = b.CreateString("2027-01-15");
    quantra::ScheduleBuilder fixedSb(b);
    fixedSb.add_effective_date(effective);
    fixedSb.add_termination_date(termination);
    fixedSb.add_calendar(quantra::enums::Calendar_TARGET);
    fixedSb.add_frequency(quantra::enums::Frequency_Annual);
    fixedSb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixedSb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixedSb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSchedule = fixedSb.Finish();

    quantra::ScheduleBuilder yoySb(b);
    yoySb.add_effective_date(effective);
    yoySb.add_termination_date(termination);
    yoySb.add_calendar(quantra::enums::Calendar_TARGET);
    yoySb.add_frequency(quantra::enums::Frequency_Annual);
    yoySb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    yoySb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    yoySb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto yoySchedule = yoySb.Finish();

    quantra::YearOnYearInflationSwapBuilder yyb(b);
    yyb.add_swap_type(quantra::enums::SwapType_Receiver);
    yyb.add_notional(1000000.0);
    yyb.add_fixed_schedule(fixedSchedule);
    yyb.add_fixed_rate(0.0204);
    yyb.add_fixed_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    yyb.add_yoy_schedule(yoySchedule);
    yyb.add_inflation_index_id(idxId);
    yyb.add_observation_lag(observationLag);
    yyb.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    yyb.add_spread(0.0002);
    yyb.add_yoy_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    yyb.add_payment_calendar(quantra::enums::Calendar_TARGET);
    yyb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto yyiis = yyb.Finish();

    quantra::PriceYearOnYearInflationSwapBuilder pyb(b);
    pyb.add_year_on_year_inflation_swap(yyiis);
    pyb.add_discounting_curve(discCurveId);
    pyb.add_inflation_curve(curveId);
    auto trade = pyb.Finish();
    auto trades = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceYearOnYearInflationSwap>>{trade});

    quantra::PriceYearOnYearInflationSwapRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_swaps(trades);
    reqb.add_include_flows(true);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceYearOnYearInflationSwapRequest>(b.GetBufferPointer());
    YearOnYearInflationSwapPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::PriceYearOnYearInflationSwapResponse>(outBuilder->GetBufferPointer());
    ASSERT_NE(resp->swaps(), nullptr);
    ASSERT_EQ(resp->swaps()->size(), 1u);
    const auto* priced = resp->swaps()->Get(0);
    ASSERT_NE(priced, nullptr);
    ASSERT_NE(priced->fixed_leg_flows(), nullptr);
    ASSERT_NE(priced->yoy_leg_flows(), nullptr);
    EXPECT_EQ(priced->fixed_leg_flows()->size(), 2u);
    EXPECT_EQ(priced->yoy_leg_flows()->size(), 2u);

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(req->pricing());
    auto discountIt = reg.rates.curves.find("DISC");
    ASSERT_NE(discountIt, reg.rates.curves.end());
    auto indexIt = reg.inflation.inflationIndices.find("EUHICP_YY");
    ASSERT_NE(indexIt, reg.inflation.inflationIndices.end());
    auto yoyIndex = std::dynamic_pointer_cast<QuantLib::YoYInflationIndex>(indexIt->second);
    ASSERT_TRUE(static_cast<bool>(yoyIndex));

    auto fixedQlSchedule = QuantLib::Schedule(
        QuantLib::Date(15, QuantLib::January, 2025),
        QuantLib::Date(15, QuantLib::January, 2027),
        QuantLib::Period(QuantLib::Annual),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward,
        false);
    auto yoyQlSchedule = fixedQlSchedule;

    auto expected = std::make_shared<QuantLib::YearOnYearInflationSwap>(
        QuantLib::YearOnYearInflationSwap::Receiver,
        1000000.0,
        fixedQlSchedule,
        0.0204,
        QuantLib::Actual365Fixed(),
        yoyQlSchedule,
        yoyIndex,
        QuantLib::Period(3, QuantLib::Months),
        QuantLib::CPI::Linear,
        0.0002,
        QuantLib::Actual365Fixed(),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing);
    QuantLib::setCouponPricer(
        expected->yoyLeg(),
        QuantLib::ext::make_shared<QuantLib::BlackYoYInflationCouponPricer>(
            QuantLib::Handle<QuantLib::YieldTermStructure>(discountIt->second->currentLink())));
    expected->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    EXPECT_NEAR(priced->npv(), expected->NPV(), 1e-8);
    EXPECT_NEAR(priced->fair_rate(), expected->fairRate(), 1e-10);
    EXPECT_NEAR(priced->fair_spread(), expected->fairSpread(), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_bps(), expected->legBPS(0), 1e-10);
    EXPECT_NEAR(priced->yoy_leg_bps(), expected->legBPS(1), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_npv(), expected->fixedLegNPV(), 1e-8);
    EXPECT_NEAR(priced->yoy_leg_npv(), expected->yoyLegNPV(), 1e-8);
}

// Variation: Payer swap, zero spread, longer 3Y schedule. Opposite swap
// direction and an extra coupon period vs the base case.
TEST_F(QuantraComparisonTest, PriceYearOnYearInflationSwap_Payer_3Y) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    auto idxId = b.CreateString("EUHICP_YY");
    auto idxFamily = b.CreateString("EU HICP YoY");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(0.0180);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(0.0190);
    auto fixNov = fixNovBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{fixOct, fixNov});

    quantra::InflationIndexSpecBuilder iisb(b);
    iisb.add_id(idxId);
    iisb.add_family_name(idxFamily);
    iisb.add_currency(idxCcy);
    iisb.add_calendar(quantra::enums::Calendar_TARGET);
    iisb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    iisb.add_frequency(quantra::enums::Frequency_Monthly);
    iisb.add_availability_lag(availabilityLag);
    iisb.add_observation_lag(observationLag);
    iisb.add_interpolated(true);
    iisb.add_revised(false);
    iisb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_YY");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);

    auto makeHelper = [&](double quote, flatbuffers::Offset<quantra::Period> tenor) {
        quantra::YearOnYearInflationSwapHelperBuilder hb(b);
        hb.add_quote_value(quote);
        hb.add_swap_observation_lag(observationLag);
        hb.add_tenor(tenor);
        hb.add_calendar(quantra::enums::Calendar_TARGET);
        hb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        hb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        hb.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
        hb.add_nominal_curve_id(discCurveId);
        auto h = hb.Finish();
        quantra::InflationPointWrapperBuilder pw(b);
        pw.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
        pw.add_point(h.Union());
        return pw.Finish();
    };
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{
        makeHelper(0.0200, p1y), makeHelper(0.0210, p2y)
    });

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_YoYInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto effective = b.CreateString("2025-01-15");
    auto termination = b.CreateString("2028-01-15");
    quantra::ScheduleBuilder fixedSb(b);
    fixedSb.add_effective_date(effective);
    fixedSb.add_termination_date(termination);
    fixedSb.add_calendar(quantra::enums::Calendar_TARGET);
    fixedSb.add_frequency(quantra::enums::Frequency_Annual);
    fixedSb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixedSb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fixedSb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSchedule = fixedSb.Finish();

    quantra::ScheduleBuilder yoySb(b);
    yoySb.add_effective_date(effective);
    yoySb.add_termination_date(termination);
    yoySb.add_calendar(quantra::enums::Calendar_TARGET);
    yoySb.add_frequency(quantra::enums::Frequency_Annual);
    yoySb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    yoySb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    yoySb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto yoySchedule = yoySb.Finish();

    quantra::YearOnYearInflationSwapBuilder yyb(b);
    yyb.add_swap_type(quantra::enums::SwapType_Payer);
    yyb.add_notional(1000000.0);
    yyb.add_fixed_schedule(fixedSchedule);
    yyb.add_fixed_rate(0.0210);
    yyb.add_fixed_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    yyb.add_yoy_schedule(yoySchedule);
    yyb.add_inflation_index_id(idxId);
    yyb.add_observation_lag(observationLag);
    yyb.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    yyb.add_spread(0.0);
    yyb.add_yoy_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    yyb.add_payment_calendar(quantra::enums::Calendar_TARGET);
    yyb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto yyiis = yyb.Finish();

    quantra::PriceYearOnYearInflationSwapBuilder pyb(b);
    pyb.add_year_on_year_inflation_swap(yyiis);
    pyb.add_discounting_curve(discCurveId);
    pyb.add_inflation_curve(curveId);
    auto trades = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceYearOnYearInflationSwap>>{pyb.Finish()});

    quantra::PriceYearOnYearInflationSwapRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_swaps(trades);
    reqb.add_include_flows(false);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceYearOnYearInflationSwapRequest>(b.GetBufferPointer());
    YearOnYearInflationSwapPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    const auto* priced =
        flatbuffers::GetRoot<quantra::PriceYearOnYearInflationSwapResponse>(outBuilder->GetBufferPointer())->swaps()->Get(0);

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(req->pricing());
    auto discountIt = reg.rates.curves.find("DISC");
    ASSERT_NE(discountIt, reg.rates.curves.end());
    auto indexIt = reg.inflation.inflationIndices.find("EUHICP_YY");
    ASSERT_NE(indexIt, reg.inflation.inflationIndices.end());
    auto yoyIndex = std::dynamic_pointer_cast<QuantLib::YoYInflationIndex>(indexIt->second);
    ASSERT_TRUE(static_cast<bool>(yoyIndex));

    auto qlSchedule = QuantLib::Schedule(
        QuantLib::Date(15, QuantLib::January, 2025),
        QuantLib::Date(15, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Annual),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward,
        false);

    auto expected = std::make_shared<QuantLib::YearOnYearInflationSwap>(
        QuantLib::YearOnYearInflationSwap::Payer,
        1000000.0,
        qlSchedule,
        0.0210,
        QuantLib::Actual365Fixed(),
        qlSchedule,
        yoyIndex,
        QuantLib::Period(3, QuantLib::Months),
        QuantLib::CPI::Linear,
        0.0,
        QuantLib::Actual365Fixed(),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing);
    QuantLib::setCouponPricer(
        expected->yoyLeg(),
        QuantLib::ext::make_shared<QuantLib::BlackYoYInflationCouponPricer>(
            QuantLib::Handle<QuantLib::YieldTermStructure>(discountIt->second->currentLink())));
    expected->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    EXPECT_NEAR(priced->npv(), expected->NPV(), 1e-8);
    EXPECT_NEAR(priced->fair_rate(), expected->fairRate(), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_npv(), expected->fixedLegNPV(), 1e-8);
    EXPECT_NEAR(priced->yoy_leg_npv(), expected->yoyLegNPV(), 1e-8);
}

}} // namespace quantra::testing
