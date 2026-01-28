/**
 * Quantra Server-Client Integration Tests
 * 
 * Tests the full gRPC round-trip: client -> server -> QuantLib -> response
 */

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <thread>
#include <chrono>
#include <memory>
#include <iostream>

#include "quantraserver.grpc.fb.h"
#include "quantraserver_generated.h"

// Request builders from examples
#include "price_fixed_rate_bond_request_generated.h"
#include "price_vanilla_swap_request_generated.h"
#include "price_fra_request_generated.h"
#include "price_cap_floor_request_generated.h"
#include "price_swaption_request_generated.h"
#include "price_cds_request_generated.h"

// Response types
#include "fixed_rate_bond_response_generated.h"
#include "vanilla_swap_response_generated.h"
#include "fra_response_generated.h"
#include "cap_floor_response_generated.h"
#include "swaption_response_generated.h"
#include "cds_response_generated.h"

namespace quantra { namespace testing {

class ServerClientTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Start server in background - assumes server is already running
        // or will be started by the test script
        channel_ = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
        stub_ = quantra::QuantraServer::NewStub(channel_);
        
        // Wait for server to be ready
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        if (!channel_->WaitForConnected(deadline)) {
            std::cerr << "WARNING: Could not connect to server at localhost:50051" << std::endl;
            std::cerr << "Make sure the server is running: ./build/server/sync_server &" << std::endl;
            serverAvailable_ = false;
        } else {
            serverAvailable_ = true;
        }
    }

    void SetUp() override {
        if (!serverAvailable_) {
            GTEST_SKIP() << "Server not available at localhost:50051";
        }
        flatRate_ = 0.03;
    }

    // Build yield curve for requests
    flatbuffers::Offset<quantra::TermStructure> buildCurve(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id) {
        
        std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;
        
        // 3M deposit
        quantra::DepositHelperBuilder dep3m(b);
        dep3m.add_rate(flatRate_);
        dep3m.add_tenor_number(3);
        dep3m.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
        dep3m.add_fixing_days(2);
        dep3m.add_calendar(quantra::enums::Calendar_TARGET);
        dep3m.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep3m.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep3m_off = dep3m.Finish();
        quantra::PointsWrapperBuilder pw3m(b);
        pw3m.add_point_type(quantra::Point_DepositHelper);
        pw3m.add_point(dep3m_off.Union());
        points_vector.push_back(pw3m.Finish());
        
        // 6M deposit
        quantra::DepositHelperBuilder dep6m(b);
        dep6m.add_rate(flatRate_);
        dep6m.add_tenor_number(6);
        dep6m.add_tenor_time_unit(quantra::enums::TimeUnit_Months);
        dep6m.add_fixing_days(2);
        dep6m.add_calendar(quantra::enums::Calendar_TARGET);
        dep6m.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep6m.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep6m_off = dep6m.Finish();
        quantra::PointsWrapperBuilder pw6m(b);
        pw6m.add_point_type(quantra::Point_DepositHelper);
        pw6m.add_point(dep6m_off.Union());
        points_vector.push_back(pw6m.Finish());
        
        // 1Y deposit
        quantra::DepositHelperBuilder dep1y(b);
        dep1y.add_rate(flatRate_);
        dep1y.add_tenor_number(1);
        dep1y.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
        dep1y.add_fixing_days(2);
        dep1y.add_calendar(quantra::enums::Calendar_TARGET);
        dep1y.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep1y.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep1y_off = dep1y.Finish();
        quantra::PointsWrapperBuilder pw1y(b);
        pw1y.add_point_type(quantra::Point_DepositHelper);
        pw1y.add_point(dep1y_off.Union());
        points_vector.push_back(pw1y.Finish());
        
        // 5Y swap
        quantra::SwapHelperBuilder sw5y(b);
        sw5y.add_rate(flatRate_);
        sw5y.add_tenor_number(5);
        sw5y.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
        sw5y.add_calendar(quantra::enums::Calendar_TARGET);
        sw5y.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        sw5y.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sw5y.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        sw5y.add_sw_floating_leg_index(quantra::enums::Ibor_Euribor6M);
        sw5y.add_spread(0.0);
        sw5y.add_fwd_start_days(0);
        auto sw5y_off = sw5y.Finish();
        quantra::PointsWrapperBuilder pw5y(b);
        pw5y.add_point_type(quantra::Point_SwapHelper);
        pw5y.add_point(sw5y_off.Union());
        points_vector.push_back(pw5y.Finish());
        
        // 10Y swap
        quantra::SwapHelperBuilder sw10y(b);
        sw10y.add_rate(flatRate_);
        sw10y.add_tenor_number(10);
        sw10y.add_tenor_time_unit(quantra::enums::TimeUnit_Years);
        sw10y.add_calendar(quantra::enums::Calendar_TARGET);
        sw10y.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        sw10y.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sw10y.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        sw10y.add_sw_floating_leg_index(quantra::enums::Ibor_Euribor6M);
        sw10y.add_spread(0.0);
        sw10y.add_fwd_start_days(0);
        auto sw10y_off = sw10y.Finish();
        quantra::PointsWrapperBuilder pw10y(b);
        pw10y.add_point_type(quantra::Point_SwapHelper);
        pw10y.add_point(sw10y_off.Union());
        points_vector.push_back(pw10y.Finish());
        
        auto points = b.CreateVector(points_vector);
        auto cid = b.CreateString(id);
        auto ref_date = b.CreateString("2025-01-15");
        
        quantra::TermStructureBuilder tsb(b);
        tsb.add_id(cid);
        tsb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        tsb.add_interpolator(quantra::enums::Interpolator_LogLinear);
        tsb.add_bootstrap_trait(quantra::enums::BootstrapTrait_Discount);
        tsb.add_reference_date(ref_date);
        tsb.add_points(points);
        return tsb.Finish();
    }
    
    flatbuffers::Offset<quantra::Index> buildIndex6M(flatbuffers::grpc::MessageBuilder& b) {
        quantra::IndexBuilder ib(b);
        ib.add_period_number(6);
        ib.add_period_time_unit(quantra::enums::TimeUnit_Months);
        ib.add_settlement_days(2);
        ib.add_calendar(quantra::enums::Calendar_TARGET);
        ib.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        ib.add_end_of_month(false);
        ib.add_day_counter(quantra::enums::DayCounter_Actual360);
        return ib.Finish();
    }
    
    flatbuffers::Offset<quantra::Yield> buildYield(flatbuffers::grpc::MessageBuilder& b) {
        quantra::YieldBuilder yb(b);
        yb.add_day_counter(quantra::enums::DayCounter_Actual360);
        yb.add_compounding(quantra::enums::Compounding_Compounded);
        yb.add_frequency(quantra::enums::Frequency_Annual);
        return yb.Finish();
    }

    static std::shared_ptr<grpc::Channel> channel_;
    static std::unique_ptr<quantra::QuantraServer::Stub> stub_;
    static bool serverAvailable_;
    double flatRate_;
};

std::shared_ptr<grpc::Channel> ServerClientTest::channel_;
std::unique_ptr<quantra::QuantraServer::Stub> ServerClientTest::stub_;
bool ServerClientTest::serverAvailable_ = false;

// ======================== FIXED RATE BOND ========================
TEST_F(ServerClientTest, FixedRateBond_RoundTrip) {
    std::cout << "\n=== Server-Client: Fixed Rate Bond ===" << std::endl;
    
    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto asof = b.CreateString("2025-01-15");
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_curves(curves);
    pb.add_bond_pricing_details(true);
    auto pricing = pb.Finish();
    
    auto eff = b.CreateString("2024-01-15");
    auto term = b.CreateString("2029-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Annual);
    sb.add_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
    sb.add_end_of_month(false);
    auto schedule = sb.Finish();
    
    auto idate = b.CreateString("2024-01-15");
    quantra::FixedRateBondBuilder bb(b);
    bb.add_settlement_days(2);
    bb.add_face_amount(100.0);
    bb.add_schedule(schedule);
    bb.add_rate(0.05);
    bb.add_accrual_day_counter(quantra::enums::DayCounter_ActualActual);
    bb.add_issue_date(idate);
    bb.add_redemption(100.0);
    bb.add_payment_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    auto bond = bb.Finish();
    
    auto yield = buildYield(b);
    auto dc = b.CreateString("discount");
    
    quantra::PriceFixedRateBondBuilder pfb(b);
    pfb.add_fixed_rate_bond(bond);
    pfb.add_discounting_curve(dc);
    pfb.add_yield(yield);
    auto pfbOff = pfb.Finish();
    
    auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>>{pfbOff});
    
    quantra::PriceFixedRateBondRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_bonds(bonds);
    b.Finish(rb.Finish());
    
    auto request = b.ReleaseMessage<quantra::PriceFixedRateBondRequest>();
    
    grpc::ClientContext context;
    flatbuffers::grpc::Message<quantra::PriceFixedRateBondResponse> response;
    
    auto status = stub_->PriceFixedRateBond(&context, request, &response);
    
    ASSERT_TRUE(status.ok()) << "gRPC call failed: " << status.error_message();
    ASSERT_TRUE(response.Verify()) << "Response verification failed";
    
    auto resp = response.GetRoot();
    ASSERT_NE(resp->bonds(), nullptr);
    ASSERT_GT(resp->bonds()->size(), 0);
    
    double npv = resp->bonds()->Get(0)->npv();
    std::cout << "NPV: " << npv << std::endl;
    
    // Should match our known value from unit tests
    EXPECT_NEAR(npv, 107.432, 0.01);
}

// ======================== VANILLA SWAP ========================
TEST_F(ServerClientTest, VanillaSwap_RoundTrip) {
    std::cout << "\n=== Server-Client: Vanilla Swap ===" << std::endl;
    
    flatbuffers::grpc::MessageBuilder b;
    double notional = 1000000.0, fixedRate = 0.035;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto asof = b.CreateString("2025-01-15");
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_curves(curves);
    auto pricing = pb.Finish();
    
    // Fixed leg
    auto feff = b.CreateString("2025-01-17");
    auto fterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();
    
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(fixedRate);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();
    
    // Float leg
    auto fleff = b.CreateString("2025-01-17");
    auto flterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();
    
    auto idx6m = buildIndex6M(b);
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();
    
    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_floating_leg(floatLeg);
    auto swap = vsb.Finish();
    
    auto dc = b.CreateString("discount");
    quantra::PriceVanillaSwapBuilder pvsb(b);
    pvsb.add_vanilla_swap(swap);
    pvsb.add_discounting_curve(dc);
    pvsb.add_forwarding_curve(dc);
    auto pvsbOff = pvsb.Finish();
    
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{pvsbOff});
    
    quantra::PriceVanillaSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());
    
    auto request = b.ReleaseMessage<quantra::PriceVanillaSwapRequest>();
    
    grpc::ClientContext context;
    flatbuffers::grpc::Message<quantra::PriceVanillaSwapResponse> response;
    
    auto status = stub_->PriceVanillaSwap(&context, request, &response);
    
    ASSERT_TRUE(status.ok()) << "gRPC call failed: " << status.error_message();
    ASSERT_TRUE(response.Verify()) << "Response verification failed";
    
    auto resp = response.GetRoot();
    ASSERT_NE(resp->swaps(), nullptr);
    ASSERT_GT(resp->swaps()->size(), 0);
    
    double npv = resp->swaps()->Get(0)->npv();
    double fairRate = resp->swaps()->Get(0)->fair_rate();
    std::cout << "NPV: " << npv << " | Fair Rate: " << fairRate * 100 << "%" << std::endl;
    
    EXPECT_NEAR(npv, -22895, 1.0);
    EXPECT_NEAR(fairRate, 0.03, 0.001);
}

// ======================== CDS ========================
TEST_F(ServerClientTest, CDS_RoundTrip) {
    std::cout << "\n=== Server-Client: CDS ===" << std::endl;
    
    flatbuffers::grpc::MessageBuilder b;
    double notional = 10000000.0, spread = 0.01, recovery = 0.40, hazard = 0.02;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto asof = b.CreateString("2025-01-15");
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_curves(curves);
    auto pricing = pb.Finish();
    
    auto eff = b.CreateString("2025-01-15");
    auto term = b.CreateString("2030-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Quarterly);
    sb.add_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    auto schedule = sb.Finish();
    
    quantra::CDSBuilder cdsb(b);
    cdsb.add_side(quantra::enums::ProtectionSide_Buyer);
    cdsb.add_notional(notional);
    cdsb.add_spread(spread);
    cdsb.add_schedule(schedule);
    cdsb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    auto cds = cdsb.Finish();
    
    quantra::CreditCurveBuilder ccb(b);
    ccb.add_recovery_rate(recovery);
    ccb.add_flat_hazard_rate(hazard);
    auto cc = ccb.Finish();
    
    auto dc = b.CreateString("discount");
    quantra::PriceCDSBuilder pcdsb(b);
    pcdsb.add_cds(cds);
    pcdsb.add_discounting_curve(dc);
    pcdsb.add_credit_curve(cc);
    auto pcdsbOff = pcdsb.Finish();
    
    auto cdss = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceCDS>>{pcdsbOff});
    
    quantra::PriceCDSRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_cds_list(cdss);
    b.Finish(rb.Finish());
    
    auto request = b.ReleaseMessage<quantra::PriceCDSRequest>();
    
    grpc::ClientContext context;
    flatbuffers::grpc::Message<quantra::PriceCDSResponse> response;
    
    auto status = stub_->PriceCDS(&context, request, &response);
    
    ASSERT_TRUE(status.ok()) << "gRPC call failed: " << status.error_message();
    ASSERT_TRUE(response.Verify()) << "Response verification failed";
    
    auto resp = response.GetRoot();
    ASSERT_NE(resp->cds_list(), nullptr);
    ASSERT_GT(resp->cds_list()->size(), 0);
    
    double npv = resp->cds_list()->Get(0)->npv();
    double fairSpread = resp->cds_list()->Get(0)->fair_spread();
    std::cout << "NPV: " << npv << " | Fair Spread: " << fairSpread * 10000 << " bps" << std::endl;
    
    EXPECT_NEAR(npv, 86698.9, 1.0);
    EXPECT_NEAR(fairSpread, 0.0118792, 0.0001);
}

// ======================== LATENCY TEST ========================
TEST_F(ServerClientTest, Latency_MultipleRequests) {
    std::cout << "\n=== Server-Client: Latency Test ===" << std::endl;
    
    const int NUM_REQUESTS = 100;
    std::vector<double> latencies;
    latencies.reserve(NUM_REQUESTS);
    
    for (int i = 0; i < NUM_REQUESTS; ++i) {
        flatbuffers::grpc::MessageBuilder b;
        
        auto ts = buildCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        auto asof = b.CreateString("2025-01-15");
        
        quantra::PricingBuilder pb(b);
        pb.add_as_of_date(asof);
        pb.add_settlement_date(asof);
        pb.add_curves(curves);
        pb.add_bond_pricing_details(true);
        auto pricing = pb.Finish();
        
        auto eff = b.CreateString("2024-01-15");
        auto term = b.CreateString("2029-01-15");
        quantra::ScheduleBuilder sb(b);
        sb.add_effective_date(eff);
        sb.add_termination_date(term);
        sb.add_calendar(quantra::enums::Calendar_TARGET);
        sb.add_frequency(quantra::enums::Frequency_Annual);
        sb.add_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
        sb.add_end_of_month(false);
        auto schedule = sb.Finish();
        
        auto idate = b.CreateString("2024-01-15");
        quantra::FixedRateBondBuilder bb(b);
        bb.add_settlement_days(2);
        bb.add_face_amount(100.0);
        bb.add_schedule(schedule);
        bb.add_rate(0.05);
        bb.add_accrual_day_counter(quantra::enums::DayCounter_ActualActual);
        bb.add_issue_date(idate);
        bb.add_redemption(100.0);
        bb.add_payment_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        auto bond = bb.Finish();
        
        auto yield = buildYield(b);
        auto dc = b.CreateString("discount");
        
        quantra::PriceFixedRateBondBuilder pfb(b);
        pfb.add_fixed_rate_bond(bond);
        pfb.add_discounting_curve(dc);
        pfb.add_yield(yield);
        auto pfbOff = pfb.Finish();
        
        auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>>{pfbOff});
        
        quantra::PriceFixedRateBondRequestBuilder rb(b);
        rb.add_pricing(pricing);
        rb.add_bonds(bonds);
        b.Finish(rb.Finish());
        
        auto request = b.ReleaseMessage<quantra::PriceFixedRateBondRequest>();
        
        grpc::ClientContext context;
        flatbuffers::grpc::Message<quantra::PriceFixedRateBondResponse> response;
        
        auto start = std::chrono::high_resolution_clock::now();
        auto status = stub_->PriceFixedRateBond(&context, request, &response);
        auto end = std::chrono::high_resolution_clock::now();
        
        ASSERT_TRUE(status.ok());
        
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
        latencies.push_back(duration);
    }
    
    // Calculate statistics
    double sum = 0, min_lat = latencies[0], max_lat = latencies[0];
    for (double lat : latencies) {
        sum += lat;
        min_lat = std::min(min_lat, lat);
        max_lat = std::max(max_lat, lat);
    }
    double avg = sum / NUM_REQUESTS;
    
    // Calculate percentiles (sort first)
    std::sort(latencies.begin(), latencies.end());
    double p50 = latencies[NUM_REQUESTS / 2];
    double p95 = latencies[static_cast<int>(NUM_REQUESTS * 0.95)];
    double p99 = latencies[static_cast<int>(NUM_REQUESTS * 0.99)];
    
    std::cout << "Latency Statistics (" << NUM_REQUESTS << " requests):" << std::endl;
    std::cout << "  Min: " << min_lat << " μs" << std::endl;
    std::cout << "  Avg: " << avg << " μs" << std::endl;
    std::cout << "  P50: " << p50 << " μs" << std::endl;
    std::cout << "  P95: " << p95 << " μs" << std::endl;
    std::cout << "  P99: " << p99 << " μs" << std::endl;
    std::cout << "  Max: " << max_lat << " μs" << std::endl;
    std::cout << "  Throughput: " << (1000000.0 / avg) << " req/sec" << std::endl;
    
    // Reasonable latency expectations
    EXPECT_LT(avg, 50000);  // Average under 50ms
    EXPECT_LT(p99, 100000); // P99 under 100ms
}

}} // namespace