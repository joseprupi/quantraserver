// Bootstrap Inflation Curves parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_ZeroRate_TenorGridMatchesPillars) {
    flatbuffers::grpc::MessageBuilder b;

    // Pricing (needs at least one discount curve)
    auto asof = b.CreateString("2025-01-15");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        buildCurve(b, "DISC")
    });
    auto indices = buildIndicesVector(b);

    // Inflation index spec
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

    // Inflation curve spec built from QuantLib ZCIIS helpers.
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

    // Query: tenor grid that matches curve conventions so sampling hits pillar nodes.
    auto t1 = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto t2 = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    auto t5 = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t1, t2, t5});

    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    tgb.add_calendar(quantra::enums::Calendar_TARGET);
    tgb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto tg = tgb.Finish();

    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();

    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_ZeroRate)
    });

    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());

    ASSERT_NE(resp->results(), nullptr);
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_NE(r, nullptr);
    ASSERT_TRUE(r->error() == nullptr);
    ASSERT_NE(r->grid_dates(), nullptr);
    ASSERT_NE(r->series(), nullptr);
    ASSERT_EQ(r->grid_dates()->size(), 3u);
    ASSERT_EQ(r->series()->size(), 1u);

    const auto* s = r->series()->Get(0);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(s->measure(), quantra::enums::InflationCurveMeasure_ZeroRate);
    ASSERT_NE(s->values(), nullptr);
    ASSERT_EQ(s->values()->size(), 3u);

    // QuantLib's inflation term structures can map a date to an inflation period
    // (e.g., month start) when computing zero rates, so the sampled values may
    // deviate slightly from the raw node quotes. We still expect them to be close.
    EXPECT_NEAR(s->values()->Get(0), 0.0200, 1.5e-3);
    EXPECT_NEAR(s->values()->Get(1), 0.0210, 1.5e-3);
    EXPECT_NEAR(s->values()->Get(2), 0.0220, 5e-4);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_ZeroHelpersCanResolveQuoteIds) {
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
    auto fixings = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Fixing>>{fixOct, fixNov, fixDec});

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

    auto q1Id = b.CreateString("HICP_ZC_1Y");
    quantra::QuoteSpecBuilder q1Builder(b);
    q1Builder.add_id(q1Id);
    q1Builder.add_kind(quantra::QuoteKind_Rate);
    q1Builder.add_quote_type(quantra::QuoteType_Curve);
    q1Builder.add_value(0.0205);
    auto q1 = q1Builder.Finish();
    auto q2Id = b.CreateString("HICP_ZC_2Y");
    quantra::QuoteSpecBuilder q2Builder(b);
    q2Builder.add_id(q2Id);
    q2Builder.add_kind(quantra::QuoteKind_Rate);
    q2Builder.add_quote_type(quantra::QuoteType_Curve);
    q2Builder.add_value(0.0215);
    auto q2 = q2Builder.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{q1, q2});

    auto curveId = b.CreateString("HICP_ZC_QID");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto p2y = buildPeriod(b, 2, quantra::enums::TimeUnit_Years);
    quantra::ZeroCouponInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_id(q1Id);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h1 = h1Builder.Finish();
    quantra::ZeroCouponInflationSwapHelperBuilder h2Builder(b);
    h2Builder.add_quote_id(q2Id);
    h2Builder.add_swap_observation_lag(observationLag);
    h2Builder.add_tenor(p2y);
    h2Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h2Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h2Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h2Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h2 = h2Builder.Finish();
    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw2Builder.add_point(h2.Union());
    auto pwh2 = pw2Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{pwh1, pwh2});

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
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{p1y, p2y});
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    tgb.add_calendar(quantra::enums::Calendar_TARGET);
    tgb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto tg = tgb.Finish();
    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();

    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_ZeroRate)
    });
    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_TRUE(r->error() == nullptr);
    const auto* s = r->series()->Get(0);
    ASSERT_EQ(s->values()->size(), 2u);
    EXPECT_NEAR(s->values()->Get(0), 0.0205, 5e-4);
    EXPECT_NEAR(s->values()->Get(1), 0.0215, 5e-4);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_YoYRate_TenorGridMatchesHelpers) {
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
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{pwh1, pwh2});

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

    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{p1y, p2y});
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    tgb.add_calendar(quantra::enums::Calendar_TARGET);
    tgb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto tg = tgb.Finish();
    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();

    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_YoYRate)
    });
    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_TRUE(r->error() == nullptr);
    const auto* s = r->series()->Get(0);
    ASSERT_EQ(s->values()->size(), 2u);
    EXPECT_NEAR(s->values()->Get(0), 0.0200, 1.5e-3);
    EXPECT_NEAR(s->values()->Get(1), 0.0210, 1.5e-3);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_MissingFixingsReturnsItemError) {
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
    auto inflationIndex = iisb.Finish();
    auto inflationIndices =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationIndexSpec>>{inflationIndex});

    auto curveId = b.CreateString("HICP_ZC_ERR");
    auto curveRef = b.CreateString("2025-01-15");
    auto discCurveId = b.CreateString("DISC");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    quantra::ZeroCouponInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    auto h1 = h1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{pwh1});

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
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{p1y});
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    auto tg = tgb.Finish();
    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();
    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_ZeroRate)
    });
    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_NE(r->error(), nullptr);
    EXPECT_NE(std::string(r->error()->error_message()->c_str()).find("base fixing unavailable"), std::string::npos);
}

TEST_F(QuantraComparisonTest, BootstrapInflationCurves_MissingNominalCurveReturnsItemError) {
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

    auto curveId = b.CreateString("HICP_YY_ERR");
    auto curveRef = b.CreateString("2025-01-15");
    auto p1y = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
    auto missingNominal = b.CreateString("MISSING_CURVE");
    quantra::YearOnYearInflationSwapHelperBuilder h1Builder(b);
    h1Builder.add_quote_value(0.0200);
    h1Builder.add_swap_observation_lag(observationLag);
    h1Builder.add_tenor(p1y);
    h1Builder.add_calendar(quantra::enums::Calendar_TARGET);
    h1Builder.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    h1Builder.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    h1Builder.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    h1Builder.add_nominal_curve_id(missingNominal);
    auto h1 = h1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_YearOnYearInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    auto points = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationPointWrapper>>{pwh1});

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
    icb.add_points(points);
    auto inflationCurve = icb.Finish();
    auto inflationCurves =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveSpec>>{inflationCurve});

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, 0, 0, 0, inflationIndices, inflationCurves);

    auto tenors = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{p1y});
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors);
    auto tg = tgb.Finish();
    quantra::DateGridSpecBuilder dgsb(b);
    dgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    dgsb.add_grid(tg.Union());
    auto grid = dgsb.Finish();
    auto measures = b.CreateVector(std::vector<int8_t>{
        static_cast<int8_t>(quantra::enums::InflationCurveMeasure_YoYRate)
    });
    quantra::InflationCurveQuerySpecBuilder qsb(b);
    qsb.add_curve_id(curveId);
    qsb.add_measures(measures);
    qsb.add_grid(grid);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::InflationCurveQuerySpec>>{query});

    quantra::BootstrapInflationCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());

    auto req = flatbuffers::GetRoot<quantra::BootstrapInflationCurvesRequest>(b.GetBufferPointer());
    BootstrapInflationCurvesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);

    auto resp =
        flatbuffers::GetRoot<quantra::BootstrapInflationCurvesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    const auto* r = resp->results()->Get(0);
    ASSERT_NE(r->error(), nullptr);
    EXPECT_NE(std::string(r->error()->error_message()->c_str()).find("nominal_curve_id"), std::string::npos);
}

}} // namespace quantra::testing
