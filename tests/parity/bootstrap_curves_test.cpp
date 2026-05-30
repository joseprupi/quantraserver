// Bootstrap Curves parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, BootstrapCurves_DiscountFactors) {
    std::cout << "\n--- Test: BootstrapCurves Discount Factors ---\n";
    
    flatbuffers::grpc::MessageBuilder b;
    
    // Build TenorGrid with 1Y, 5Y, 10Y
    std::vector<flatbuffers::Offset<quantra::Period>> tenors;
    
    quantra::PeriodBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    
    quantra::PeriodBuilder t5y(b);
    t5y.add_n(5); t5y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t5y.Finish());
    
    quantra::PeriodBuilder t10y(b);
    t10y.add_n(10); t10y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t10y.Finish());
    
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::DateGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_DF)};
    auto measures = b.CreateVector(measures_vec);
    
    auto curve_id = b.CreateString("test_curve");
    quantra::CurveQuerySpecBuilder cqb(b);
    cqb.add_curve_id(curve_id);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::CurveQuerySpec>> queries_vec;
    queries_vec.push_back(query);
    auto queries = b.CreateVector(queries_vec);

    auto curve = buildCurve(b, "test_curve");
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves_vec;
    curves_vec.push_back(curve);
    auto curves = b.CreateVector(curves_vec);
    
    // IndexDefs needed by SwapHelpers
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, as_of, 0, 0, indices, 0, curves);

    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());
    
    auto request = flatbuffers::GetRoot<quantra::BootstrapCurvesRequest>(b.GetBufferPointer());
    BootstrapCurvesRequestHandler handler;
    auto response_builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto response_offset = handler.request(response_builder, request);
    response_builder->Finish(response_offset);
    
    auto response = flatbuffers::GetRoot<quantra::BootstrapCurvesResponse>(response_builder->GetBufferPointer());
    
    ASSERT_EQ(response->results()->size(), 1u);
    auto result = response->results()->Get(0);
    ASSERT_EQ(result->error(), nullptr);
    ASSERT_EQ(result->series()->size(), 1u);
    
    auto df_series = result->series()->Get(0);
    ASSERT_EQ(df_series->measure(), quantra::CurveMeasure_DF);
    ASSERT_EQ(df_series->values()->size(), 3u);
    
    QuantLib::Date refDate = bootstrappedCurve_->referenceDate();
    std::vector<QuantLib::Date> testDates = {
        refDate + 1 * QuantLib::Years,
        refDate + 5 * QuantLib::Years,
        refDate + 10 * QuantLib::Years
    };
    
    for (size_t i = 0; i < testDates.size(); i++) {
        double quantra_df = df_series->values()->Get(i);
        double ql_df = bootstrappedCurve_->discount(testDates[i]);
        std::cout << "  " << testDates[i] << ": QL=" << std::fixed << std::setprecision(8) 
                  << ql_df << ", Quantra=" << quantra_df 
                  << ", Diff=" << std::scientific << std::abs(ql_df - quantra_df) << std::endl;
        EXPECT_NEAR(quantra_df, ql_df, 1e-10);
    }
}

TEST_F(QuantraComparisonTest, BootstrapCurves_ZeroRates) {
    std::cout << "\n--- Test: BootstrapCurves Zero Rates ---\n";
    
    flatbuffers::grpc::MessageBuilder b;
    
    std::vector<flatbuffers::Offset<quantra::Period>> tenors;
    
    quantra::PeriodBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    
    quantra::PeriodBuilder t5y(b);
    t5y.add_n(5); t5y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t5y.Finish());
    
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::DateGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_ZERO)};
    auto measures = b.CreateVector(measures_vec);
    
    auto curve_id = b.CreateString("test_curve");
    quantra::CurveQuerySpecBuilder cqb(b);
    cqb.add_curve_id(curve_id);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::CurveQuerySpec>> queries_vec;
    queries_vec.push_back(query);
    auto queries = b.CreateVector(queries_vec);

    auto curve = buildCurve(b, "test_curve");
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves_vec;
    curves_vec.push_back(curve);
    auto curves = b.CreateVector(curves_vec);
    
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, as_of, 0, 0, indices, 0, curves);

    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());
    
    auto request = flatbuffers::GetRoot<quantra::BootstrapCurvesRequest>(b.GetBufferPointer());
    BootstrapCurvesRequestHandler handler;
    auto response_builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto response_offset = handler.request(response_builder, request);
    response_builder->Finish(response_offset);
    
    auto response = flatbuffers::GetRoot<quantra::BootstrapCurvesResponse>(response_builder->GetBufferPointer());
    
    auto result = response->results()->Get(0);
    ASSERT_EQ(result->error(), nullptr);
    
    auto zero_series = result->series()->Get(0);
    ASSERT_EQ(zero_series->measure(), quantra::CurveMeasure_ZERO);
    
    QuantLib::Date refDate = bootstrappedCurve_->referenceDate();
    std::vector<QuantLib::Date> testDates = {
        refDate + 1 * QuantLib::Years,
        refDate + 5 * QuantLib::Years
    };
    
    for (size_t i = 0; i < testDates.size(); i++) {
        double quantra_zero = zero_series->values()->Get(i);
        double ql_zero = bootstrappedCurve_->zeroRate(testDates[i], QuantLib::Actual365Fixed(), 
                                                       QuantLib::Continuous).rate();
        std::cout << "  " << testDates[i] << ": QL=" << std::fixed << std::setprecision(8) 
                  << ql_zero << ", Quantra=" << quantra_zero 
                  << ", Diff=" << std::scientific << std::abs(ql_zero - quantra_zero) << std::endl;
        EXPECT_NEAR(quantra_zero, ql_zero, 1e-10);
    }
}

TEST_F(QuantraComparisonTest, BootstrapCurves_PillarDates) {
    std::cout << "\n--- Test: BootstrapCurves Pillar Dates ---\n";
    
    flatbuffers::grpc::MessageBuilder b;
    
    std::vector<flatbuffers::Offset<quantra::Period>> tenors;
    quantra::PeriodBuilder t1y(b);
    t1y.add_n(1); t1y.add_unit(quantra::enums::TimeUnit_Years);
    tenors.push_back(t1y.Finish());
    auto tenors_vec = b.CreateVector(tenors);
    
    quantra::TenorGridBuilder tgb(b);
    tgb.add_tenors(tenors_vec);
    auto tenor_grid = tgb.Finish();
    
    quantra::DateGridSpecBuilder cgsb(b);
    cgsb.add_grid_type(quantra::DateGrid_TenorGrid);
    cgsb.add_grid(tenor_grid.Union());
    auto grid_spec = cgsb.Finish();
    
    std::vector<int8_t> measures_vec = {static_cast<int8_t>(quantra::CurveMeasure_DF)};
    auto measures = b.CreateVector(measures_vec);
    
    auto curve_id = b.CreateString("test_curve");
    quantra::CurveQuerySpecBuilder cqb(b);
    cqb.add_curve_id(curve_id);
    cqb.add_measures(measures);
    cqb.add_grid(grid_spec);
    auto query = cqb.Finish();
    
    std::vector<flatbuffers::Offset<quantra::CurveQuerySpec>> queries_vec;
    queries_vec.push_back(query);
    auto queries = b.CreateVector(queries_vec);

    auto curve = buildCurve(b, "test_curve");
    std::vector<flatbuffers::Offset<quantra::TermStructure>> curves_vec;
    curves_vec.push_back(curve);
    auto curves = b.CreateVector(curves_vec);
    
    auto indices = buildIndicesVector(b);
    
    auto as_of = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, as_of, 0, 0, indices, 0, curves);

    quantra::BootstrapCurvesRequestBuilder reqb(b);
    reqb.add_pricing(pricing);
    reqb.add_queries(queries);
    b.Finish(reqb.Finish());
    
    auto request = flatbuffers::GetRoot<quantra::BootstrapCurvesRequest>(b.GetBufferPointer());
    BootstrapCurvesRequestHandler handler;
    auto response_builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto response_offset = handler.request(response_builder, request);
    response_builder->Finish(response_offset);
    
    auto response = flatbuffers::GetRoot<quantra::BootstrapCurvesResponse>(response_builder->GetBufferPointer());
    
    auto result = response->results()->Get(0);
    ASSERT_NE(result->pillar_dates(), nullptr);
    
    std::cout << "  Pillar dates count: " << result->pillar_dates()->size() << std::endl;
    for (size_t i = 0; i < result->pillar_dates()->size(); i++) {
        std::cout << "    " << result->pillar_dates()->Get(i)->c_str() << std::endl;
    }
    EXPECT_GE(result->pillar_dates()->size(), 6u);
}

}} // namespace quantra::testing
