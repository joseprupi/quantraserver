// Zero Coupon Inflation Swap parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, PriceZeroCouponInflationSwap_MatchesQuantLib) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    auto idxId = b.CreateString("EUHICP");
    auto idxFamily = b.CreateString("EU HICP");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    auto fixingDec = b.CreateString("2024-12-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(100.0);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(100.2);
    auto fixNov = fixNovBuilder.Finish();
    quantra::FixingBuilder fixDecBuilder(b);
    fixDecBuilder.add_date(fixingDec);
    fixDecBuilder.add_value(100.4);
    auto fixDec = fixDecBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{
        fixOct, fixNov, fixDec
    });

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
    iisb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_ZC");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    auto p5y = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);

    quantra::ZeroCouponInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h1 = h1Builder.Finish();

    quantra::ZeroCouponInflationSwapHelperBuilder h2Builder(b);
    h2Builder.add_quote_value(0.0210);
    h2Builder.add_swap_observation_lag(observationLag);
    h2Builder.add_tenor(p2y);
    h2Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h2Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h2Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h2Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h2 = h2Builder.Finish();

    quantra::ZeroCouponInflationSwapHelperBuilder h5Builder(b);
    h5Builder.add_quote_value(0.0220);
    h5Builder.add_swap_observation_lag(observationLag);
    h5Builder.add_tenor(p5y);
    h5Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h5Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h5Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h5Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h5 = h5Builder.Finish();

    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw2Builder.add_point(h2.Union());
    auto pwh2 = pw2Builder.Finish();
    quantra::InflationPointWrapperBuilder pw5Builder(b);
    pw5Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw5Builder.add_point(h5.Union());
    auto pwh5 = pw5Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{
        pwh1, pwh2, pwh5
    });

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_allow_extrapolation(true);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto startDate = b.CreateString("2025-01-15");
    auto maturityDate = b.CreateString("2030-01-15");
    quantra::ZeroCouponInflationSwapBuilder zcib(b);
    zcib.add_swap_type(quantra::enums::SwapType_Payer);
    zcib.add_notional(1000000.0);
    zcib.add_start_date(startDate);
    zcib.add_maturity_date(maturityDate);
    zcib.add_fixed_calendar(quantra::enums::Calendar_TARGET);
    zcib.add_fixed_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    zcib.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    zcib.add_fixed_rate(0.0217);
    zcib.add_inflation_index_id(idxId);
    zcib.add_observation_lag(observationLag);
    zcib.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    zcib.add_adjust_observation_dates(false);
    zcib.add_inflation_calendar(quantra::enums::Calendar_NullCalendar);
    zcib.add_inflation_convention(quantra::enums::BusinessDayConvention_Following);
    auto zciis = zcib.Finish();

    quantra::PriceZeroCouponInflationSwapBuilder pzb(b);
    pzb.add_zero_coupon_inflation_swap(zciis);
    pzb.add_discounting_curve(discCurveId);
    pzb.add_inflation_curve(curveId);
    auto trade = pzb.Finish();
    auto trades = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceZeroCouponInflationSwap>>{trade});

    quantra::PriceZeroCouponInflationSwapRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_swaps(trades);
    reqb.add_include_flows(true);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceZeroCouponInflationSwapRequest>(b.GetBufferPointer());
    ZeroCouponInflationSwapPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::PriceZeroCouponInflationSwapResponse>(outBuilder->GetBufferPointer());
    ASSERT_NE(resp->swaps(), nullptr);
    ASSERT_EQ(resp->swaps()->size(), 1u);
    const auto* priced = resp->swaps()->Get(0);
    ASSERT_NE(priced, nullptr);
    ASSERT_NE(priced->fixed_leg_flows(), nullptr);
    ASSERT_NE(priced->inflation_leg_flows(), nullptr);
    EXPECT_EQ(priced->fixed_leg_flows()->size(), 1u);
    EXPECT_EQ(priced->inflation_leg_flows()->size(), 1u);

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(req->pricing());
    auto discountIt = reg.rates.curves.find("DISC");
    ASSERT_NE(discountIt, reg.rates.curves.end());
    auto indexIt = reg.inflation.inflationIndices.find("EUHICP");
    ASSERT_NE(indexIt, reg.inflation.inflationIndices.end());
    auto zeroIndex = std::dynamic_pointer_cast<QuantLib::ZeroInflationIndex>(indexIt->second);
    ASSERT_TRUE(static_cast<bool>(zeroIndex));

    auto expected = std::make_shared<QuantLib::ZeroCouponInflationSwap>(
        QuantLib::ZeroCouponInflationSwap::Payer,
        1000000.0,
        QuantLib::Date(15, QuantLib::January, 2025),
        QuantLib::Date(15, QuantLib::January, 2030),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing,
        QuantLib::Actual365Fixed(),
        0.0217,
        zeroIndex,
        QuantLib::Period(3, QuantLib::Months),
        QuantLib::CPI::Linear,
        false,
        QuantLib::NullCalendar(),
        QuantLib::Following);
    expected->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    EXPECT_NEAR(priced->npv(), expected->NPV(), 1e-8);
    EXPECT_NEAR(priced->fair_rate(), expected->fairRate(), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_bps(), expected->fixedLegBPS(), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_npv(), expected->fixedLegNPV(), 1e-8);
    EXPECT_NEAR(priced->inflation_leg_npv(), expected->inflationLegNPV(), 1e-8);
}

// Variation: Receiver swap, Flat (CPI::Flat) observation interpolation, and a
// shorter 3Y maturity. Exercises the non-Linear observation-interpolation path
// and the opposite swap direction from the base case.
TEST_F(QuantraComparisonTest, PriceZeroCouponInflationSwap_Receiver_Flat) {
    flatbuffers::grpc::MessageBuilder b;

    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    auto idxId = b.CreateString("EUHICP");
    auto idxFamily = b.CreateString("EU HICP");
    auto idxCcy = b.CreateString("EUR");
    auto availabilityLag = buildPeriod(b, 2, quantra::enums::TimeUnit_Months);
    auto observationLag = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);

    auto fixingOct = b.CreateString("2024-10-01");
    auto fixingNov = b.CreateString("2024-11-01");
    auto fixingDec = b.CreateString("2024-12-01");
    quantra::FixingBuilder fixOctBuilder(b);
    fixOctBuilder.add_date(fixingOct);
    fixOctBuilder.add_value(100.0);
    auto fixOct = fixOctBuilder.Finish();
    quantra::FixingBuilder fixNovBuilder(b);
    fixNovBuilder.add_date(fixingNov);
    fixNovBuilder.add_value(100.2);
    auto fixNov = fixNovBuilder.Finish();
    quantra::FixingBuilder fixDecBuilder(b);
    fixDecBuilder.add_date(fixingDec);
    fixDecBuilder.add_value(100.4);
    auto fixDec = fixDecBuilder.Finish();
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{
        fixOct, fixNov, fixDec
    });

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
    iisb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    iisb.add_fixings(fixings);
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_ZC");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    auto p5y = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);

    auto makeHelper = [&](double quote, flatbuffers::Offset<quantra::Period> tenor) {
        quantra::ZeroCouponInflationSwapHelperBuilder hb(b);
        hb.add_quote_value(quote);
        hb.add_swap_observation_lag(observationLag);
        hb.add_tenor(tenor);
        hb.add_calendar(quantra::enums::Calendar_TARGET);
        hb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        hb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        hb.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
        auto h = hb.Finish();
        quantra::InflationPointWrapperBuilder pw(b);
        pw.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
        pw.add_point(h.Union());
        return pw.Finish();
    };
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{
        makeHelper(0.0200, p1y), makeHelper(0.0210, p2y), makeHelper(0.0220, p5y)
    });

    quantra::InflationCurveSpecBuilder icb(b);
    icb.add_id(curveId);
    icb.add_reference_date(curveRef);
    icb.add_calendar(quantra::enums::Calendar_TARGET);
    icb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    icb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    icb.add_interpolator(quantra::enums::Interpolator_Linear);
    icb.add_bootstrap_accuracy(1.0e-12);
    icb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_allow_extrapolation(true);
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto startDate = b.CreateString("2025-01-15");
    auto maturityDate = b.CreateString("2028-01-15");
    quantra::ZeroCouponInflationSwapBuilder zcib(b);
    zcib.add_swap_type(quantra::enums::SwapType_Receiver);
    zcib.add_notional(1000000.0);
    zcib.add_start_date(startDate);
    zcib.add_maturity_date(maturityDate);
    zcib.add_fixed_calendar(quantra::enums::Calendar_TARGET);
    zcib.add_fixed_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    zcib.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    zcib.add_fixed_rate(0.0210);
    zcib.add_inflation_index_id(idxId);
    zcib.add_observation_lag(observationLag);
    zcib.add_observation_interpolation(quantra::enums::CPIInterpolationType_Flat);
    zcib.add_adjust_observation_dates(false);
    zcib.add_inflation_calendar(quantra::enums::Calendar_NullCalendar);
    zcib.add_inflation_convention(quantra::enums::BusinessDayConvention_Following);
    auto zciis = zcib.Finish();

    quantra::PriceZeroCouponInflationSwapBuilder pzb(b);
    pzb.add_zero_coupon_inflation_swap(zciis);
    pzb.add_discounting_curve(discCurveId);
    pzb.add_inflation_curve(curveId);
    auto trades = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceZeroCouponInflationSwap>>{pzb.Finish()});

    quantra::PriceZeroCouponInflationSwapRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_swaps(trades);
    reqb.add_include_flows(false);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::PriceZeroCouponInflationSwapRequest>(b.GetBufferPointer());
    ZeroCouponInflationSwapPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    const auto* priced =
        flatbuffers::GetRoot<quantra::PriceZeroCouponInflationSwapResponse>(outBuilder->GetBufferPointer())->swaps()->Get(0);

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(req->pricing());
    auto discountIt = reg.rates.curves.find("DISC");
    ASSERT_NE(discountIt, reg.rates.curves.end());
    auto indexIt = reg.inflation.inflationIndices.find("EUHICP");
    ASSERT_NE(indexIt, reg.inflation.inflationIndices.end());
    auto zeroIndex = std::dynamic_pointer_cast<QuantLib::ZeroInflationIndex>(indexIt->second);
    ASSERT_TRUE(static_cast<bool>(zeroIndex));

    auto expected = std::make_shared<QuantLib::ZeroCouponInflationSwap>(
        QuantLib::ZeroCouponInflationSwap::Receiver,
        1000000.0,
        QuantLib::Date(15, QuantLib::January, 2025),
        QuantLib::Date(15, QuantLib::January, 2028),
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing,
        QuantLib::Actual365Fixed(),
        0.0210,
        zeroIndex,
        QuantLib::Period(3, QuantLib::Months),
        QuantLib::CPI::Flat,
        false,
        QuantLib::NullCalendar(),
        QuantLib::Following);
    expected->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(*discountIt->second));

    EXPECT_NEAR(priced->npv(), expected->NPV(), 1e-8);
    EXPECT_NEAR(priced->fair_rate(), expected->fairRate(), 1e-10);
    EXPECT_NEAR(priced->fixed_leg_npv(), expected->fixedLegNPV(), 1e-8);
    EXPECT_NEAR(priced->inflation_leg_npv(), expected->inflationLegNPV(), 1e-8);
}

}} // namespace quantra::testing
