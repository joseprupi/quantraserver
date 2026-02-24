/**
 * Quantra Server-Client Integration Tests
 * Updated for new schema: volatilities in Pricing, enums in quantra::enums::
 */

#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <thread>
#include <chrono>
#include <memory>
#include <iostream>
#include <cmath>

#include "quantraserver.grpc.fb.h"
#include "quantraserver_generated.h"
#include "price_fixed_rate_bond_request_generated.h"
#include "price_vanilla_swap_request_generated.h"
#include "price_ois_swap_request_generated.h"
#include "price_basis_swap_request_generated.h"
#include "price_cds_request_generated.h"
#include "fixed_rate_bond_response_generated.h"
#include "vanilla_swap_response_generated.h"
#include "ois_swap_response_generated.h"
#include "basis_swap_response_generated.h"
#include "cds_response_generated.h"
#include "calendar_business_days_request_generated.h"
#include "calendar_business_days_response_generated.h"
#include "calendar_holidays_request_generated.h"
#include "calendar_holidays_response_generated.h"
#include "calendar_advance_request_generated.h"
#include "calendar_advance_response_generated.h"
#include "index_generated.h"

namespace quantra { namespace testing {

class ServerClientTest : public ::testing::Test {
protected:
    flatbuffers::Offset<quantra::Period> buildPeriod(
        flatbuffers::grpc::MessageBuilder& b, int n, quantra::enums::TimeUnit unit) {
        quantra::PeriodBuilder pb(b);
        pb.add_n(n);
        pb.add_unit(unit);
        return pb.Finish();
    }

    static void SetUpTestSuite() {
        channel_ = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
        stub_ = quantra::QuantraServer::NewStub(channel_);
        auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(5);
        serverAvailable_ = channel_->WaitForConnected(deadline);
        if (serverAvailable_) std::cout << "Connected to server" << std::endl;
    }

    void SetUp() override {
        if (!serverAvailable_) GTEST_SKIP() << "Server not available";
        flatRate_ = 0.03;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    flatbuffers::Offset<quantra::TermStructure> buildCurve(flatbuffers::grpc::MessageBuilder& b, const std::string& id) {
        std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;
        
        auto dep3mTenor = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);
        quantra::DepositHelperBuilder dep3m(b);
        dep3m.add_rate(flatRate_); dep3m.add_tenor(dep3mTenor);
        dep3m.add_fixing_days(2); dep3m.add_calendar(quantra::enums::Calendar_TARGET);
        dep3m.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep3m.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep3m_off = dep3m.Finish();
        quantra::PointsWrapperBuilder pw3m(b);
        pw3m.add_point_type(quantra::Point_DepositHelper);
        pw3m.add_point(dep3m_off.Union());
        points_vector.push_back(pw3m.Finish());
        
        auto dep6mTenor = buildPeriod(b, 6, quantra::enums::TimeUnit_Months);
        quantra::DepositHelperBuilder dep6m(b);
        dep6m.add_rate(flatRate_); dep6m.add_tenor(dep6mTenor);
        dep6m.add_fixing_days(2); dep6m.add_calendar(quantra::enums::Calendar_TARGET);
        dep6m.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep6m.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep6m_off = dep6m.Finish();
        quantra::PointsWrapperBuilder pw6m(b);
        pw6m.add_point_type(quantra::Point_DepositHelper);
        pw6m.add_point(dep6m_off.Union());
        points_vector.push_back(pw6m.Finish());
        
        auto dep1yTenor = buildPeriod(b, 1, quantra::enums::TimeUnit_Years);
        quantra::DepositHelperBuilder dep1y(b);
        dep1y.add_rate(flatRate_); dep1y.add_tenor(dep1yTenor);
        dep1y.add_fixing_days(2); dep1y.add_calendar(quantra::enums::Calendar_TARGET);
        dep1y.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        dep1y.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        auto dep1y_off = dep1y.Finish();
        quantra::PointsWrapperBuilder pw1y(b);
        pw1y.add_point_type(quantra::Point_DepositHelper);
        pw1y.add_point(dep1y_off.Union());
        points_vector.push_back(pw1y.Finish());
        
        auto float_idx_5y = buildIndexRef(b, "EUR_6M");
        auto sw5yTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
        quantra::SwapHelperBuilder sw5y(b);
        sw5y.add_rate(flatRate_); sw5y.add_tenor(sw5yTenor);
        sw5y.add_calendar(quantra::enums::Calendar_TARGET);
        sw5y.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        sw5y.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sw5y.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        sw5y.add_float_index(float_idx_5y);
        sw5y.add_spread(0.0); sw5y.add_fwd_start_days(0);
        auto sw5y_off = sw5y.Finish();
        quantra::PointsWrapperBuilder pw5y(b);
        pw5y.add_point_type(quantra::Point_SwapHelper);
        pw5y.add_point(sw5y_off.Union());
        points_vector.push_back(pw5y.Finish());
        
        auto float_idx_10y = buildIndexRef(b, "EUR_6M");
        auto sw10yTenor = buildPeriod(b, 10, quantra::enums::TimeUnit_Years);
        quantra::SwapHelperBuilder sw10y(b);
        sw10y.add_rate(flatRate_); sw10y.add_tenor(sw10yTenor);
        sw10y.add_calendar(quantra::enums::Calendar_TARGET);
        sw10y.add_sw_fixed_leg_frequency(quantra::enums::Frequency_Annual);
        sw10y.add_sw_fixed_leg_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sw10y.add_sw_fixed_leg_day_counter(quantra::enums::DayCounter_Thirty360);
        sw10y.add_float_index(float_idx_10y);
        sw10y.add_spread(0.0); sw10y.add_fwd_start_days(0);
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
    
    flatbuffers::Offset<quantra::IndexDef> buildIndexDef_EUR6M(
        flatbuffers::grpc::MessageBuilder& b) {
        auto id = b.CreateString("EUR_6M");
        auto name = b.CreateString("Euribor");
        auto ccy = b.CreateString("EUR");
        auto tenor = buildPeriod(b, 6, quantra::enums::TimeUnit_Months);
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Ibor);
        idb.add_tenor(tenor);
        idb.add_fixing_days(2);
        idb.add_calendar(quantra::enums::Calendar_TARGET);
        idb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        idb.add_day_counter(quantra::enums::DayCounter_Actual360);
        idb.add_end_of_month(false);
        idb.add_currency(ccy);
        return idb.Finish();
    }

    flatbuffers::Offset<quantra::IndexDef> buildIndexDef_EUR3M(
        flatbuffers::grpc::MessageBuilder& b) {
        auto id = b.CreateString("EUR_3M");
        auto name = b.CreateString("Euribor");
        auto ccy = b.CreateString("EUR");
        auto tenor = buildPeriod(b, 3, quantra::enums::TimeUnit_Months);
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Ibor);
        idb.add_tenor(tenor);
        idb.add_fixing_days(2);
        idb.add_calendar(quantra::enums::Calendar_TARGET);
        idb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        idb.add_day_counter(quantra::enums::DayCounter_Actual360);
        idb.add_end_of_month(false);
        idb.add_currency(ccy);
        return idb.Finish();
    }

    flatbuffers::Offset<quantra::IndexDef> buildIndexDef_SOFR(
        flatbuffers::grpc::MessageBuilder& b) {
        auto id = b.CreateString("USD_SOFR");
        auto name = b.CreateString("SOFR");
        auto ccy = b.CreateString("USD");
        auto tenor = buildPeriod(b, 0, quantra::enums::TimeUnit_Days);
        quantra::IndexDefBuilder idb(b);
        idb.add_id(id);
        idb.add_name(name);
        idb.add_index_type(quantra::IndexType_Overnight);
        idb.add_tenor(tenor);
        idb.add_fixing_days(0);
        idb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
        idb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
        idb.add_day_counter(quantra::enums::DayCounter_Actual360);
        idb.add_currency(ccy);
        return idb.Finish();
    }

    flatbuffers::Offset<quantra::IndexRef> buildIndexRef(
        flatbuffers::grpc::MessageBuilder& b, const std::string& refId) {
        auto sid = b.CreateString(refId);
        quantra::IndexRefBuilder irb(b);
        irb.add_id(sid);
        return irb.Finish();
    }

    flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>>
    buildIndicesVector(flatbuffers::grpc::MessageBuilder& b) {
        std::vector<flatbuffers::Offset<quantra::IndexDef>> defs;
        defs.push_back(buildIndexDef_EUR3M(b));
        defs.push_back(buildIndexDef_EUR6M(b));
        defs.push_back(buildIndexDef_SOFR(b));
        return b.CreateVector(defs);
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

TEST_F(ServerClientTest, FixedRateBond_RoundTrip) {
    std::cout << "\n=== Server-Client: Fixed Rate Bond ===" << std::endl;
    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof); pb.add_settlement_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves); pb.add_bond_pricing_details(true);
    auto pricing = pb.Finish();
    
    auto eff = b.CreateString("2024-01-15");
    auto term = b.CreateString("2029-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff); sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Annual);
    sb.add_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
    sb.add_end_of_month(false);
    auto schedule = sb.Finish();
    
    auto idate = b.CreateString("2024-01-15");
    quantra::FixedRateBondBuilder bb(b);
    bb.add_settlement_days(2); bb.add_face_amount(100.0);
    bb.add_schedule(schedule); bb.add_rate(0.05);
    bb.add_accrual_day_counter(quantra::enums::DayCounter_ActualActual);
    bb.add_issue_date(idate); bb.add_redemption(100.0);
    bb.add_payment_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    auto bond = bb.Finish();
    
    auto yield = buildYield(b);
    auto dc = b.CreateString("discount");
    quantra::PriceFixedRateBondBuilder pfb(b);
    pfb.add_fixed_rate_bond(bond); pfb.add_discounting_curve(dc); pfb.add_yield(yield);
    auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>>{pfb.Finish()});
    
    quantra::PriceFixedRateBondRequestBuilder rb(b);
    rb.add_pricing(pricing); rb.add_bonds(bonds);
    b.Finish(rb.Finish());
    
    auto request = b.ReleaseMessage<quantra::PriceFixedRateBondRequest>();
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::PriceFixedRateBondResponse> response;
    auto status = stub_->PriceFixedRateBond(&context, request, &response);
    
    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    double npv = response.GetRoot()->bonds()->Get(0)->npv();
    std::cout << "NPV: " << npv << std::endl;
    EXPECT_NEAR(npv, 107.432, 0.01);
}

TEST_F(ServerClientTest, VanillaSwap_RoundTrip) {
    std::cout << "\n=== Server-Client: Vanilla Swap ===" << std::endl;
    flatbuffers::grpc::MessageBuilder b;
    double notional = 1000000.0, fixedRate = 0.035;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices2 = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto credit_id = b.CreateString("credit");
    auto discount_id = b.CreateString("discount");
    auto ref_date = b.CreateString("2025-01-15");

    quantra::CdsHelperConventionsBuilder hcb(b);
    hcb.add_settlement_days(0);
    hcb.add_frequency(quantra::enums::Frequency_Quarterly);
    hcb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    hcb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    hcb.add_last_period_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    hcb.add_settles_accrual(true);
    hcb.add_pays_at_default_time(true);
    hcb.add_rebates_accrual(true);
    hcb.add_helper_model(quantra::enums::CdsHelperModel_MidPoint);
    auto helper_conv = hcb.Finish();
    auto empty_quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CdsQuote>>{});

    quantra::CreditCurveSpecBuilder ccb(b);
    ccb.add_id(credit_id);
    ccb.add_reference_date(ref_date);
    ccb.add_calendar(quantra::enums::Calendar_TARGET);
    ccb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    ccb.add_recovery_rate(0.40);
    ccb.add_curve_interpolator(quantra::enums::Interpolator_LogLinear);
    ccb.add_helper_conventions(helper_conv);
    ccb.add_quotes(empty_quotes);
    ccb.add_flat_hazard_rate(0.02);
    auto cc = ccb.Finish();
    auto credit_curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CreditCurveSpec>>{cc});

    quantra::CdsModelSpecBuilder cmsb(b);
    cmsb.add_engine_type(quantra::enums::CdsEngineType_MidPoint);
    auto cds_payload = cmsb.Finish();
    auto model_id = b.CreateString("cds_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(model_id);
    msb.add_payload_type(quantra::ModelPayload_CdsModelSpec);
    msb.add_payload(cds_payload.Union());
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{msb.Finish()});

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof); pb.add_settlement_date(asof);
    pb.add_indices(indices2);
    pb.add_curves(curves);
    pb.add_credit_curves(credit_curves);
    pb.add_models(models);
    auto pricing = pb.Finish();
    
    auto feff = b.CreateString("2025-01-17"); auto fterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff); fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();
    
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional); flb.add_schedule(fixedSch); flb.add_rate(fixedRate);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();
    
    auto fleff = b.CreateString("2025-01-17"); auto flterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff); flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto floatSch = flsb.Finish();
    
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional); flgb.add_schedule(floatSch); flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360); flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();
    
    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg); vsb.add_floating_leg(floatLeg);
    auto swap = vsb.Finish();
    
    auto dc = b.CreateString("discount");
    quantra::PriceVanillaSwapBuilder pvsb(b);
    pvsb.add_vanilla_swap(swap); pvsb.add_discounting_curve(dc); pvsb.add_forwarding_curve(dc);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{pvsb.Finish()});
    
    quantra::PriceVanillaSwapRequestBuilder rb(b);
    rb.add_pricing(pricing); rb.add_swaps(swaps);
    b.Finish(rb.Finish());
    
    auto request = b.ReleaseMessage<quantra::PriceVanillaSwapRequest>();
    ASSERT_TRUE(request.Verify());
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::PriceVanillaSwapResponse> response;
    auto status = stub_->PriceVanillaSwap(&context, request, &response);
    
    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    auto r = response.GetRoot()->swaps()->Get(0);
    std::cout << "NPV: " << r->npv() << " | Fair Rate: " << r->fair_rate()*100 << "%" << std::endl;
    EXPECT_NEAR(r->npv(), -22895, 1.0);
    EXPECT_NEAR(r->fair_rate(), 0.03, 0.001);
}

TEST_F(ServerClientTest, OisSwap_RoundTrip) {
    std::cout << "\n=== Server-Client: OIS Swap ===" << std::endl;
    flatbuffers::grpc::MessageBuilder b;
    const double notional = 1000000.0;

    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    auto pricing = pb.Finish();

    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");

    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(eff);
    fsb.add_termination_date(term);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder fixedB(b);
    fixedB.add_notional(notional);
    fixedB.add_schedule(fixedSch);
    fixedB.add_rate(0.03);
    fixedB.add_day_counter(quantra::enums::DayCounter_Actual360);
    fixedB.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = fixedB.Finish();

    quantra::ScheduleBuilder osb(b);
    osb.add_effective_date(eff);
    osb.add_termination_date(term);
    osb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    osb.add_frequency(quantra::enums::Frequency_Annual);
    osb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto overnightSch = osb.Finish();

    auto sofr = buildIndexRef(b, "USD_SOFR");
    quantra::OisFloatingLegBuilder ob(b);
    ob.add_notional(notional);
    ob.add_schedule(overnightSch);
    ob.add_index(sofr);
    ob.add_spread(0.0);
    ob.add_day_counter(quantra::enums::DayCounter_Actual360);
    ob.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    ob.add_payment_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    ob.add_averaging_method(quantra::enums::RateAveragingType_Compound);
    auto overnightLeg = ob.Finish();

    quantra::OisSwapBuilder sb(b);
    sb.add_swap_type(quantra::enums::SwapType_Payer);
    sb.add_fixed_leg(fixedLeg);
    sb.add_overnight_leg(overnightLeg);
    auto swap = sb.Finish();

    auto discount = b.CreateString("discount");
    quantra::PriceOisSwapBuilder psb(b);
    psb.add_ois_swap(swap);
    psb.add_discounting_curve(discount);
    psb.add_forwarding_curve(discount);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceOisSwap>>{psb.Finish()});

    quantra::PriceOisSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    auto request = b.ReleaseMessage<quantra::PriceOisSwapRequest>();
    ASSERT_TRUE(request.Verify());
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::PriceOisSwapResponse> response;
    auto status = stub_->PriceOisSwap(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    auto r = response.GetRoot()->swaps()->Get(0);
    EXPECT_TRUE(std::isfinite(r->npv()));
    EXPECT_TRUE(std::isfinite(r->fair_rate()));
}

TEST_F(ServerClientTest, BasisSwap_RoundTrip) {
    std::cout << "\n=== Server-Client: Basis Swap ===" << std::endl;
    flatbuffers::grpc::MessageBuilder b;
    const double notional = 1000000.0;

    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_indices(indices);
    pb.add_curves(curves);
    auto pricing = pb.Finish();

    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");

    quantra::ScheduleBuilder l1b(b);
    l1b.add_effective_date(eff);
    l1b.add_termination_date(term);
    l1b.add_calendar(quantra::enums::Calendar_TARGET);
    l1b.add_frequency(quantra::enums::Frequency_Quarterly);
    l1b.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    l1b.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    l1b.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto sch1 = l1b.Finish();

    quantra::ScheduleBuilder l2b(b);
    l2b.add_effective_date(eff);
    l2b.add_termination_date(term);
    l2b.add_calendar(quantra::enums::Calendar_TARGET);
    l2b.add_frequency(quantra::enums::Frequency_Semiannual);
    l2b.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    l2b.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    l2b.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto sch2 = l2b.Finish();

    auto eur3m = buildIndexRef(b, "EUR_3M");
    quantra::SwapFloatingLegBuilder leg1b(b);
    leg1b.add_notional(notional);
    leg1b.add_schedule(sch1);
    leg1b.add_index(eur3m);
    leg1b.add_spread(0.0);
    leg1b.add_day_counter(quantra::enums::DayCounter_Actual360);
    leg1b.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto leg1 = leg1b.Finish();

    auto eur6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder leg2b(b);
    leg2b.add_notional(notional);
    leg2b.add_schedule(sch2);
    leg2b.add_index(eur6m);
    leg2b.add_spread(0.0);
    leg2b.add_day_counter(quantra::enums::DayCounter_Actual360);
    leg2b.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto leg2 = leg2b.Finish();

    quantra::BasisSwapBuilder bsb(b);
    bsb.add_swap_type(quantra::enums::SwapType_Payer);
    bsb.add_leg1(leg1);
    bsb.add_leg2(leg2);
    auto basisSwap = bsb.Finish();

    auto discount = b.CreateString("discount");
    quantra::PriceBasisSwapBuilder psb(b);
    psb.add_basis_swap(basisSwap);
    psb.add_discounting_curve(discount);
    psb.add_forwarding_curve_leg1(discount);
    psb.add_forwarding_curve_leg2(discount);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceBasisSwap>>{psb.Finish()});

    quantra::PriceBasisSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    auto request = b.ReleaseMessage<quantra::PriceBasisSwapRequest>();
    ASSERT_TRUE(request.Verify());
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::PriceBasisSwapResponse> response;
    auto status = stub_->PriceBasisSwap(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    auto r = response.GetRoot()->swaps()->Get(0);
    EXPECT_TRUE(std::isfinite(r->npv()));
    EXPECT_TRUE(std::isfinite(r->leg1_npv()));
    EXPECT_TRUE(std::isfinite(r->leg2_npv()));
}

TEST_F(ServerClientTest, CDS_RoundTrip) {
    std::cout << "\n=== Server-Client: CDS ===" << std::endl;
    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices3 = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto credit_id = b.CreateString("credit");
    auto discount_id = b.CreateString("discount");
    auto ref_date = b.CreateString("2025-01-15");

    quantra::CdsHelperConventionsBuilder hcb(b);
    hcb.add_settlement_days(0);
    hcb.add_frequency(quantra::enums::Frequency_Quarterly);
    hcb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    hcb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    hcb.add_last_period_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    hcb.add_settles_accrual(true);
    hcb.add_pays_at_default_time(true);
    hcb.add_rebates_accrual(true);
    hcb.add_helper_model(quantra::enums::CdsHelperModel_MidPoint);
    auto helper_conv = hcb.Finish();
    auto empty_quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CdsQuote>>{});

    quantra::CreditCurveSpecBuilder ccb(b);
    ccb.add_id(credit_id);
    ccb.add_reference_date(ref_date);
    ccb.add_calendar(quantra::enums::Calendar_TARGET);
    ccb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    ccb.add_recovery_rate(0.40);
    ccb.add_curve_interpolator(quantra::enums::Interpolator_LogLinear);
    ccb.add_helper_conventions(helper_conv);
    ccb.add_quotes(empty_quotes);
    ccb.add_flat_hazard_rate(0.02);
    auto cc = ccb.Finish();
    auto credit_curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CreditCurveSpec>>{cc});

    quantra::CdsModelSpecBuilder cmsb(b);
    cmsb.add_engine_type(quantra::enums::CdsEngineType_MidPoint);
    auto cds_payload = cmsb.Finish();
    auto model_id = b.CreateString("cds_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(model_id);
    msb.add_payload_type(quantra::ModelPayload_CdsModelSpec);
    msb.add_payload(cds_payload.Union());
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{msb.Finish()});

    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof); pb.add_settlement_date(asof);
    pb.add_indices(indices3);
    pb.add_curves(curves);
    pb.add_credit_curves(credit_curves);
    pb.add_models(models);
    auto pricing = pb.Finish();
    
    auto eff = b.CreateString("2025-01-15"); auto term = b.CreateString("2030-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff); sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Quarterly);
    sb.add_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    auto schedule = sb.Finish();
    
    quantra::CDSBuilder cdsb(b);
    cdsb.add_side(quantra::enums::ProtectionSide_Buyer);
    cdsb.add_notional(10000000.0); cdsb.add_running_coupon(0.01);
    cdsb.add_schedule(schedule);
    cdsb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    auto cds = cdsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceCDSBuilder pcdsb(b);
    pcdsb.add_cds(cds);
    pcdsb.add_discounting_curve(dc);
    pcdsb.add_credit_curve_id(credit_id);
    pcdsb.add_model(model_id);
    auto cdss = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceCDS>>{pcdsb.Finish()});
    
    quantra::PriceCDSRequestBuilder rb(b);
    rb.add_pricing(pricing); rb.add_cds_list(cdss);
    b.Finish(rb.Finish());
    
    auto request = b.ReleaseMessage<quantra::PriceCDSRequest>();
    ASSERT_TRUE(request.Verify());
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::PriceCDSResponse> response;
    auto status = stub_->PriceCDS(&context, request, &response);
    
    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    auto r = response.GetRoot()->cds_list()->Get(0);
    std::cout << "NPV: " << r->npv() << " | Fair Spread: " << r->fair_spread()*10000 << " bps" << std::endl;
    EXPECT_NEAR(r->npv(), 86698.9, 1.0);
    EXPECT_NEAR(r->fair_spread(), 0.0118792, 0.0001);
}

TEST_F(ServerClientTest, CalendarBusinessDays_RoundTrip) {
    flatbuffers::grpc::MessageBuilder b;
    auto start = b.CreateString("2025/01/01");
    auto end = b.CreateString("2025/01/10");

    quantra::CalendarBusinessDaysRequestBuilder rb(b);
    rb.add_calendar(quantra::enums::Calendar_TARGET);
    rb.add_start_date(start);
    rb.add_end_date(end);
    rb.add_include_start(true);
    rb.add_include_end(true);
    b.Finish(rb.Finish());

    auto request = b.ReleaseMessage<quantra::CalendarBusinessDaysRequest>();
    ASSERT_TRUE(request.Verify());

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::CalendarBusinessDaysResponse> response;
    auto status = stub_->CalendarBusinessDays(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    auto root = response.GetRoot();
    ASSERT_NE(root, nullptr);
    ASSERT_NE(root->dates(), nullptr);
    EXPECT_GT(root->count(), 0u);

    bool hasJan1 = false;
    bool hasJan2 = false;
    for (flatbuffers::uoffset_t i = 0; i < root->dates()->size(); ++i) {
        const std::string d = root->dates()->Get(i)->str();
        if (d == "2025-01-01") hasJan1 = true;
        if (d == "2025-01-02") hasJan2 = true;
    }
    EXPECT_FALSE(hasJan1);
    EXPECT_TRUE(hasJan2);
}

TEST_F(ServerClientTest, CalendarHolidays_ExcludeAndIncludeWeekends) {
    // Excluding weekends: this range has weekend-only non-business days for TARGET.
    {
        flatbuffers::grpc::MessageBuilder b;
        auto start = b.CreateString("2025/01/03");
        auto end = b.CreateString("2025/01/06");

        quantra::CalendarHolidaysRequestBuilder rb(b);
        rb.add_calendar(quantra::enums::Calendar_TARGET);
        rb.add_start_date(start);
        rb.add_end_date(end);
        rb.add_include_weekends(false);
        b.Finish(rb.Finish());

        auto request = b.ReleaseMessage<quantra::CalendarHolidaysRequest>();
        ASSERT_TRUE(request.Verify());

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        flatbuffers::grpc::Message<quantra::CalendarHolidaysResponse> response;
        auto status = stub_->CalendarHolidays(&context, request, &response);

        ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
        auto root = response.GetRoot();
        ASSERT_NE(root, nullptr);
        EXPECT_EQ(root->count(), 0u);
    }

    // Including weekends: should return Saturday and Sunday.
    {
        flatbuffers::grpc::MessageBuilder b;
        auto start = b.CreateString("2025/01/03");
        auto end = b.CreateString("2025/01/06");

        quantra::CalendarHolidaysRequestBuilder rb(b);
        rb.add_calendar(quantra::enums::Calendar_TARGET);
        rb.add_start_date(start);
        rb.add_end_date(end);
        rb.add_include_weekends(true);
        b.Finish(rb.Finish());

        auto request = b.ReleaseMessage<quantra::CalendarHolidaysRequest>();
        ASSERT_TRUE(request.Verify());

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        flatbuffers::grpc::Message<quantra::CalendarHolidaysResponse> response;
        auto status = stub_->CalendarHolidays(&context, request, &response);

        ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
        auto root = response.GetRoot();
        ASSERT_NE(root, nullptr);
        ASSERT_NE(root->dates(), nullptr);
        EXPECT_EQ(root->count(), 2u);
    }
}

TEST_F(ServerClientTest, CalendarAdvance_PositiveAndNegativeDays) {
    // +3 business days from Friday 2025-01-03 -> Wednesday 2025-01-08
    {
        flatbuffers::grpc::MessageBuilder b;
        auto date = b.CreateString("2025/01/03");

        quantra::CalendarAdvanceRequestBuilder rb(b);
        rb.add_calendar(quantra::enums::Calendar_TARGET);
        rb.add_date(date);
        rb.add_tenor_number(3);
        rb.add_tenor_unit(quantra::enums::TimeUnit_Days);
        rb.add_convention(quantra::enums::BusinessDayConvention_Following);
        rb.add_end_of_month(false);
        b.Finish(rb.Finish());

        auto request = b.ReleaseMessage<quantra::CalendarAdvanceRequest>();
        ASSERT_TRUE(request.Verify());

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        flatbuffers::grpc::Message<quantra::CalendarAdvanceResponse> response;
        auto status = stub_->CalendarAdvance(&context, request, &response);

        ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
        auto root = response.GetRoot();
        ASSERT_NE(root, nullptr);
        ASSERT_NE(root->advanced_date(), nullptr);
        EXPECT_EQ(root->advanced_date()->str(), "2025-01-08");
    }

    // -2 business days from Monday 2025-01-06 -> Thursday 2025-01-02
    {
        flatbuffers::grpc::MessageBuilder b;
        auto date = b.CreateString("2025/01/06");

        quantra::CalendarAdvanceRequestBuilder rb(b);
        rb.add_calendar(quantra::enums::Calendar_TARGET);
        rb.add_date(date);
        rb.add_tenor_number(-2);
        rb.add_tenor_unit(quantra::enums::TimeUnit_Days);
        rb.add_convention(quantra::enums::BusinessDayConvention_Following);
        rb.add_end_of_month(false);
        b.Finish(rb.Finish());

        auto request = b.ReleaseMessage<quantra::CalendarAdvanceRequest>();
        ASSERT_TRUE(request.Verify());

        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
        flatbuffers::grpc::Message<quantra::CalendarAdvanceResponse> response;
        auto status = stub_->CalendarAdvance(&context, request, &response);

        ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
        auto root = response.GetRoot();
        ASSERT_NE(root, nullptr);
        ASSERT_NE(root->advanced_date(), nullptr);
        EXPECT_EQ(root->advanced_date()->str(), "2025-01-02");
    }
}

TEST_F(ServerClientTest, CalendarBusinessDays_InvalidRangeFails) {
    flatbuffers::grpc::MessageBuilder b;
    auto start = b.CreateString("2025/01/10");
    auto end = b.CreateString("2025/01/01");

    quantra::CalendarBusinessDaysRequestBuilder rb(b);
    rb.add_calendar(quantra::enums::Calendar_TARGET);
    rb.add_start_date(start);
    rb.add_end_date(end);
    b.Finish(rb.Finish());

    auto request = b.ReleaseMessage<quantra::CalendarBusinessDaysRequest>();
    ASSERT_TRUE(request.Verify());

    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::CalendarBusinessDaysResponse> response;
    auto status = stub_->CalendarBusinessDays(&context, request, &response);

    EXPECT_FALSE(status.ok());
}

TEST_F(ServerClientTest, Latency_MultipleRequests) {
    std::cout << "\n=== Server-Client: Latency Test ===" << std::endl;
    const int NUM = 50;
    std::vector<double> latencies;
    int failures = 0;
    
    for (int i = 0; i < NUM; ++i) {
        flatbuffers::grpc::MessageBuilder b;
        auto ts = buildCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        auto lat_indices = buildIndicesVector(b);
        auto asof = b.CreateString("2025-01-15");
        quantra::PricingBuilder pb(b);
        pb.add_as_of_date(asof); pb.add_settlement_date(asof);
        pb.add_indices(lat_indices);
        pb.add_curves(curves);
        auto pricing = pb.Finish();
        
        auto eff = b.CreateString("2024-01-15"); auto term = b.CreateString("2029-01-15");
        quantra::ScheduleBuilder sb(b);
        sb.add_effective_date(eff); sb.add_termination_date(term);
        sb.add_calendar(quantra::enums::Calendar_TARGET);
        sb.add_frequency(quantra::enums::Frequency_Annual);
        sb.add_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
        auto schedule = sb.Finish();
        
        auto idate = b.CreateString("2024-01-15");
        quantra::FixedRateBondBuilder bb(b);
        bb.add_settlement_days(2); bb.add_face_amount(100.0);
        bb.add_schedule(schedule); bb.add_rate(0.05);
        bb.add_accrual_day_counter(quantra::enums::DayCounter_ActualActual);
        bb.add_issue_date(idate); bb.add_redemption(100.0);
        bb.add_payment_convention(quantra::enums::BusinessDayConvention_Unadjusted);
        auto bond = bb.Finish();
        
        auto yield = buildYield(b);
        auto dc = b.CreateString("discount");
        quantra::PriceFixedRateBondBuilder pfb(b);
        pfb.add_fixed_rate_bond(bond); pfb.add_discounting_curve(dc); pfb.add_yield(yield);
        auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>>{pfb.Finish()});
        
        quantra::PriceFixedRateBondRequestBuilder rb(b);
        rb.add_pricing(pricing); rb.add_bonds(bonds);
        b.Finish(rb.Finish());
        
        auto request = b.ReleaseMessage<quantra::PriceFixedRateBondRequest>();
        grpc::ClientContext context;
        context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
        flatbuffers::grpc::Message<quantra::PriceFixedRateBondResponse> response;
        
        auto start = std::chrono::high_resolution_clock::now();
        auto status = stub_->PriceFixedRateBond(&context, request, &response);
        auto end = std::chrono::high_resolution_clock::now();
        
        if (status.ok()) latencies.push_back(std::chrono::duration_cast<std::chrono::microseconds>(end-start).count());
        else failures++;
    }
    
    ASSERT_GT(latencies.size(), 0u);
    std::sort(latencies.begin(), latencies.end());
    double sum = 0; for (auto l : latencies) sum += l;
    std::cout << "Latency: Avg=" << sum/latencies.size() << "μs, P50=" << latencies[latencies.size()/2] 
              << "μs, P99=" << latencies[latencies.size()*99/100] << "μs" << std::endl;
    EXPECT_LT(sum/latencies.size(), 50000);
}

}} // namespace
