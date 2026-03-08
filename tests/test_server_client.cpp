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
#include "price_zero_coupon_inflation_swap_request_generated.h"
#include "price_year_on_year_inflation_swap_request_generated.h"
#include "price_ois_swap_request_generated.h"
#include "price_basis_swap_request_generated.h"
#include "price_cds_request_generated.h"
#include "fixed_rate_bond_response_generated.h"
#include "vanilla_swap_response_generated.h"
#include "zero_coupon_inflation_swap_response_generated.h"
#include "year_on_year_inflation_swap_response_generated.h"
#include "ois_swap_response_generated.h"
#include "basis_swap_response_generated.h"
#include "cds_response_generated.h"
#include "calendar_business_days_request_generated.h"
#include "calendar_business_days_response_generated.h"
#include "calendar_holidays_request_generated.h"
#include "calendar_holidays_response_generated.h"
#include "calendar_advance_request_generated.h"
#include "calendar_advance_response_generated.h"
#include "bootstrap_inflation_curves_request_generated.h"
#include "bootstrap_inflation_curves_response_generated.h"
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

    flatbuffers::Offset<quantra::Pricing> buildPricing(
        flatbuffers::grpc::MessageBuilder& b,
        flatbuffers::Offset<flatbuffers::String> asOfDate,
        flatbuffers::Offset<flatbuffers::String> settlementDate = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>> quotes = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>> indices = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapIndexDef>>> swapIndices = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>>> curves = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CouponPricer>>> couponPricers = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::CreditCurveSpec>>> creditCurves = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>> volSurfaces = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::ModelSpec>>> models = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::EquityUnderlyingSpec>>> equityUnderlyings = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::InflationIndexSpec>>> inflationIndices = 0,
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::InflationCurveSpec>>> inflationCurves = 0,
        bool bondPricingDetails = false,
        bool bondPricingFlows = false,
        bool swaptionPricingDetails = false,
        bool swaptionPricingRebump = false) {
        auto rates = (indices.o != 0 || swapIndices.o != 0 || curves.o != 0 || couponPricers.o != 0)
            ? quantra::CreateRatesMarketData(b, indices, swapIndices, curves, couponPricers)
            : 0;
        auto credit = creditCurves.o != 0 ? quantra::CreateCreditMarketData(b, creditCurves) : 0;
        auto volatility = (volSurfaces.o != 0 || models.o != 0)
            ? quantra::CreateVolatilityMarketData(b, volSurfaces, models)
            : 0;
        auto equity = equityUnderlyings.o != 0 ? quantra::CreateEquityMarketData(b, equityUnderlyings) : 0;
        auto inflation = (inflationIndices.o != 0 || inflationCurves.o != 0)
            ? quantra::CreateInflationMarketData(b, inflationIndices, inflationCurves)
            : 0;
        auto options = (bondPricingDetails || bondPricingFlows || swaptionPricingDetails || swaptionPricingRebump)
            ? quantra::CreatePricingOptions(
                  b,
                  bondPricingDetails,
                  bondPricingFlows,
                  swaptionPricingDetails,
                  swaptionPricingRebump)
            : 0;

        return quantra::CreatePricing(
            b,
            asOfDate,
            settlementDate,
            quotes,
            rates,
            credit,
            volatility,
            equity,
            inflation,
            options);
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
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, 0, 0, 0, 0, 0, 0, 0, true);
    
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

    auto pricing = buildPricing(b, asof, asof, 0, indices2, 0, curves, 0, credit_curves, 0, models);
    
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

    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);

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

    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);

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

    auto pricing = buildPricing(b, asof, asof, 0, indices3, 0, curves, 0, credit_curves, 0, models);
    
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

TEST_F(ServerClientTest, BootstrapInflationCurves_RoundTrip) {
    flatbuffers::grpc::MessageBuilder b;

    auto ts = buildCurve(b, "DISC");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

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

    quantra::InflationPointWrapperBuilder pw1Builder(b);
    pw1Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
    pw1Builder.add_point(h1.Union());
    auto pwh1 = pw1Builder.Finish();
    quantra::InflationPointWrapperBuilder pw2Builder(b);
    pw2Builder.add_point_type(quantra::InflationPoint_ZeroCouponInflationSwapHelper);
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
    icb.add_kind(quantra::enums::InflationCurveKind_ZeroInflation);
    icb.add_index_id(idxId);
    icb.add_discount_curve_id(discCurveId);
    icb.add_allow_extrapolation(true);
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

    auto request = b.ReleaseMessage<quantra::BootstrapInflationCurvesRequest>();
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::BootstrapInflationCurvesResponse> response;
    auto status = stub_->BootstrapInflationCurves(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    const auto* results = response.GetRoot()->results();
    ASSERT_NE(results, nullptr);
    ASSERT_EQ(results->size(), 1u);
    const auto* result = results->Get(0);
    ASSERT_NE(result, nullptr);
    ASSERT_EQ(result->error(), nullptr);
    ASSERT_NE(result->series(), nullptr);
    ASSERT_EQ(result->series()->size(), 1u);
    ASSERT_NE(result->pillar_dates(), nullptr);
    EXPECT_EQ(result->pillar_dates()->size(), 2u);

    const auto* series = result->series()->Get(0);
    ASSERT_NE(series, nullptr);
    ASSERT_NE(series->values(), nullptr);
    ASSERT_EQ(series->values()->size(), 2u);
    EXPECT_NEAR(series->values()->Get(0), 0.0200, 5e-4);
    EXPECT_NEAR(series->values()->Get(1), 0.0210, 5e-4);
}

TEST_F(ServerClientTest, PriceZeroCouponInflationSwap_RoundTrip) {
    flatbuffers::grpc::MessageBuilder b;

    auto ts = buildCurve(b, "DISC");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

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
    zcib.add_nominal(1000000.0);
    zcib.add_start_date(startDate);
    zcib.add_maturity_date(maturityDate);
    zcib.add_fixed_rate(0.0217);
    zcib.add_fixed_calendar(quantra::enums::Calendar_TARGET);
    zcib.add_fixed_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    zcib.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    zcib.add_inflation_index_id(idxId);
    zcib.add_observation_lag(observationLag);
    zcib.add_observation_interpolation(quantra::enums::CPIInterpolationType_Linear);
    zcib.add_inflation_calendar(quantra::enums::Calendar_NullCalendar);
    zcib.add_inflation_convention(quantra::enums::BusinessDayConvention_Following);
    auto swap = zcib.Finish();

    quantra::PriceZeroCouponInflationSwapBuilder pzb(b);
    pzb.add_zero_coupon_inflation_swap(swap);
    pzb.add_discounting_curve(discCurveId);
    pzb.add_inflation_curve(curveId);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceZeroCouponInflationSwap>>{pzb.Finish()});

    quantra::PriceZeroCouponInflationSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    rb.add_include_flows(true);
    b.Finish(rb.Finish());

    auto request = b.ReleaseMessage<quantra::PriceZeroCouponInflationSwapRequest>();
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::PriceZeroCouponInflationSwapResponse> response;
    auto status = stub_->PriceZeroCouponInflationSwap(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    const auto* swapsOut = response.GetRoot()->swaps();
    ASSERT_NE(swapsOut, nullptr);
    ASSERT_EQ(swapsOut->size(), 1u);
    const auto* out = swapsOut->Get(0);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(std::isfinite(out->npv()));
    EXPECT_TRUE(std::isfinite(out->fair_rate()));
    ASSERT_NE(out->fixed_leg_flows(), nullptr);
    ASSERT_NE(out->inflation_leg_flows(), nullptr);
    EXPECT_EQ(out->fixed_leg_flows()->size(), 1u);
    EXPECT_EQ(out->inflation_leg_flows()->size(), 1u);
}

TEST_F(ServerClientTest, PriceYearOnYearInflationSwap_RoundTrip) {
    flatbuffers::grpc::MessageBuilder b;

    auto ts = buildCurve(b, "DISC");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

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
    yyb.add_nominal(1000000.0);
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
    auto swap = yyb.Finish();

    quantra::PriceYearOnYearInflationSwapBuilder pyb(b);
    pyb.add_year_on_year_inflation_swap(swap);
    pyb.add_discounting_curve(discCurveId);
    pyb.add_inflation_curve(curveId);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceYearOnYearInflationSwap>>{pyb.Finish()});

    quantra::PriceYearOnYearInflationSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    rb.add_include_flows(true);
    b.Finish(rb.Finish());

    auto request = b.ReleaseMessage<quantra::PriceYearOnYearInflationSwapRequest>();
    grpc::ClientContext context;
    context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(10));
    flatbuffers::grpc::Message<quantra::PriceYearOnYearInflationSwapResponse> response;
    auto status = stub_->PriceYearOnYearInflationSwap(&context, request, &response);

    ASSERT_TRUE(status.ok()) << "gRPC failed: " << status.error_message();
    const auto* swapsOut = response.GetRoot()->swaps();
    ASSERT_NE(swapsOut, nullptr);
    ASSERT_EQ(swapsOut->size(), 1u);
    const auto* out = swapsOut->Get(0);
    ASSERT_NE(out, nullptr);
    EXPECT_TRUE(std::isfinite(out->npv()));
    EXPECT_TRUE(std::isfinite(out->fair_rate()));
    EXPECT_TRUE(std::isfinite(out->fair_spread()));
    ASSERT_NE(out->fixed_leg_flows(), nullptr);
    ASSERT_NE(out->yoy_leg_flows(), nullptr);
    EXPECT_EQ(out->fixed_leg_flows()->size(), 2u);
    EXPECT_EQ(out->yoy_leg_flows()->size(), 2u);
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
        auto pricing = buildPricing(b, asof, asof, 0, lat_indices, 0, curves);
        
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
