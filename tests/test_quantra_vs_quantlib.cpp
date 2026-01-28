/**
 * Quantra vs QuantLib Comparison Tests
 */

#include <gtest/gtest.h>
#include <ql/quantlib.hpp>
#include <iostream>
#include <iomanip>

#include "fixed_rate_bond_pricing_request.h"
#include "vanilla_swap_pricing_request.h"
#include "fra_pricing_request.h"
#include "cap_floor_pricing_request.h"
#include "swaption_pricing_request.h"
#include "cds_pricing_request.h"

#include "price_fixed_rate_bond_request_generated.h"
#include "fixed_rate_bond_response_generated.h"
#include "price_vanilla_swap_request_generated.h"
#include "vanilla_swap_response_generated.h"
#include "price_fra_request_generated.h"
#include "fra_response_generated.h"
#include "price_cap_floor_request_generated.h"
#include "cap_floor_response_generated.h"
#include "price_swaption_request_generated.h"
#include "swaption_response_generated.h"
#include "price_cds_request_generated.h"
#include "cds_response_generated.h"

namespace quantra { namespace testing {

class QuantraComparisonTest : public ::testing::Test {
protected:
    void SetUp() override {
        evaluationDate_ = QuantLib::Date(15, QuantLib::January, 2025);
        QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
        flatRate_ = 0.03;
        buildBootstrappedCurve();
    }
    
    void buildBootstrappedCurve() {
        std::vector<std::shared_ptr<QuantLib::RateHelper>> instruments;
        
        instruments.push_back(std::make_shared<QuantLib::DepositRateHelper>(
            flatRate_, 3 * QuantLib::Months, 2, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, true, QuantLib::Actual365Fixed()));
        
        instruments.push_back(std::make_shared<QuantLib::DepositRateHelper>(
            flatRate_, 6 * QuantLib::Months, 2, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, true, QuantLib::Actual365Fixed()));
        
        instruments.push_back(std::make_shared<QuantLib::DepositRateHelper>(
            flatRate_, 1 * QuantLib::Years, 2, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, true, QuantLib::Actual365Fixed()));
        
        auto euribor6m = std::make_shared<QuantLib::Euribor6M>();
        instruments.push_back(std::make_shared<QuantLib::SwapRateHelper>(
            flatRate_, 5 * QuantLib::Years, QuantLib::TARGET(), QuantLib::Annual,
            QuantLib::ModifiedFollowing, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), euribor6m));
        
        instruments.push_back(std::make_shared<QuantLib::SwapRateHelper>(
            flatRate_, 10 * QuantLib::Years, QuantLib::TARGET(), QuantLib::Annual,
            QuantLib::ModifiedFollowing, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), euribor6m));
        
        bootstrappedCurve_ = std::make_shared<QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
            evaluationDate_, instruments, QuantLib::Actual365Fixed());
        
        discountHandle_ = QuantLib::Handle<QuantLib::YieldTermStructure>(bootstrappedCurve_);
        forwardHandle_ = discountHandle_;
    }

    flatbuffers::Offset<quantra::TermStructure> buildCurve(
        flatbuffers::grpc::MessageBuilder& b, const std::string& id) {
        
        std::vector<flatbuffers::Offset<quantra::PointsWrapper>> points_vector;
        
        // 3M deposit - finish each builder completely before starting wrapper
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
    
    flatbuffers::Offset<quantra::Index> buildIndex3M(flatbuffers::grpc::MessageBuilder& b) {
        quantra::IndexBuilder ib(b);
        ib.add_period_number(3);
        ib.add_period_time_unit(quantra::enums::TimeUnit_Months);
        ib.add_settlement_days(2);
        ib.add_calendar(quantra::enums::Calendar_TARGET);
        ib.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        ib.add_end_of_month(false);
        ib.add_day_counter(quantra::enums::DayCounter_Actual360);
        return ib.Finish();
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
    
    flatbuffers::Offset<quantra::VolatilityTermStructure> buildVolatility(
        flatbuffers::grpc::MessageBuilder& b, double vol) {
        auto vol_id = b.CreateString("vol");
        auto ref_date = b.CreateString("2025-01-15");
        quantra::VolatilityTermStructureBuilder vb(b);
        vb.add_id(vol_id);
        vb.add_reference_date(ref_date);
        vb.add_calendar(quantra::enums::Calendar_TARGET);
        vb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        vb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
        vb.add_volatility_type(quantra::VolatilityType_ShiftedLognormal);
        vb.add_constant_vol(vol);
        return vb.Finish();
    }

    QuantLib::Date evaluationDate_;
    double flatRate_;
    std::shared_ptr<QuantLib::YieldTermStructure> bootstrappedCurve_;
    QuantLib::Handle<QuantLib::YieldTermStructure> discountHandle_, forwardHandle_;
};

// ======================== FIXED RATE BOND ========================
TEST_F(QuantraComparisonTest, FixedRateBond_NPVMatches) {
    std::cout << "\n=== Fixed Rate Bond ===" << std::endl;
    double face = 100.0, coupon = 0.05;
    QuantLib::Date issue(15,QuantLib::January,2024), mat(15,QuantLib::January,2029);
    
    QuantLib::Schedule sch(issue, mat, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::Unadjusted, QuantLib::Unadjusted, QuantLib::DateGeneration::Backward, false);
    auto qlBond = std::make_shared<QuantLib::FixedRateBond>(2, face, sch,
        std::vector<QuantLib::Rate>(1, coupon), QuantLib::ActualActual(QuantLib::ActualActual::ISDA));
    qlBond->setPricingEngine(std::make_shared<QuantLib::DiscountingBondEngine>(discountHandle_));
    double qlNPV = qlBond->NPV();

    flatbuffers::grpc::MessageBuilder b;
    
    // Build all child objects first, before any table builders
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
    bb.add_face_amount(face);
    bb.add_schedule(schedule);
    bb.add_rate(coupon);
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
    
    FixedRateBondPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFixedRateBondRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceFixedRateBondResponse>(respB->GetBufferPointer())->bonds()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

// ======================== VANILLA SWAP ========================
TEST_F(QuantraComparisonTest, VanillaSwap_NPVMatches) {
    std::cout << "\n=== Vanilla Swap ===" << std::endl;
    double notional = 1000000.0, fixedRate = 0.035;
    QuantLib::Date start = evaluationDate_ + 2, end = start + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule fixSch(start, end, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(start, end, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto qlSwap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, fixedRate, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    double qlNPV = qlSwap->NPV();
    double qlFairRate = qlSwap->fairRate();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto asof = b.CreateString("2025-01-15");
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_curves(curves);
    auto pricing = pb.Finish();
    
    // Fixed leg schedule
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
    
    // Float leg schedule
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
    
    VanillaSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceVanillaSwapRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto r = flatbuffers::GetRoot<quantra::PriceVanillaSwapResponse>(respB->GetBufferPointer())->swaps()->Get(0);
    double qNPV = r->npv();
    double qFairRate = r->fair_rate();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    std::cout << "QuantLib Fair: " << qlFairRate*100 << "% | Quantra: " << qFairRate*100 << "%" << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFairRate, qFairRate, 1e-6);
}

// ======================== FRA ========================
TEST_F(QuantraComparisonTest, FRA_NPVMatches) {
    std::cout << "\n=== FRA ===" << std::endl;
    double notional = 1000000.0, strike = 0.032;
    QuantLib::Date valDate = evaluationDate_ + QuantLib::Period(3, QuantLib::Months);
    QuantLib::Date matDate = evaluationDate_ + QuantLib::Period(6, QuantLib::Months);
    
    auto idx = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    auto qlFRA = std::make_shared<QuantLib::ForwardRateAgreement>(
        valDate, matDate, QuantLib::Position::Long, strike, notional, idx, discountHandle_);
    double qlNPV = qlFRA->NPV();
    double qlFwd = qlFRA->forwardRate();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto asof = b.CreateString("2025-01-15");
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_curves(curves);
    auto pricing = pb.Finish();
    
    auto vd = b.CreateString("2025-04-15");
    auto md = b.CreateString("2025-07-15");
    auto idx3m = buildIndex3M(b);
    
    quantra::FRABuilder fb(b);
    fb.add_start_date(vd);
    fb.add_maturity_date(md);
    fb.add_fra_type(quantra::FRAType_Long);
    fb.add_strike(strike);
    fb.add_notional(notional);
    fb.add_index(idx3m);
    fb.add_day_counter(quantra::enums::DayCounter_Actual360);
    fb.add_calendar(quantra::enums::Calendar_TARGET);
    fb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fra = fb.Finish();
    
    auto dc = b.CreateString("discount");
    quantra::PriceFRABuilder pfb(b);
    pfb.add_fra(fra);
    pfb.add_discounting_curve(dc);
    pfb.add_forwarding_curve(dc);
    auto pfbOff = pfb.Finish();
    
    auto fras = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFRA>>{pfbOff});
    
    quantra::PriceFRARequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_fras(fras);
    b.Finish(rb.Finish());
    
    FRAPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFRARequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto r = flatbuffers::GetRoot<quantra::PriceFRAResponse>(respB->GetBufferPointer())->fras()->Get(0);
    double qNPV = r->npv();
    double qFwd = r->forward_rate();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    std::cout << "QuantLib Fwd: " << qlFwd*100 << "% | Quantra: " << qFwd*100 << "%" << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFwd, qFwd, 1e-6);
}

// ======================== CAP ========================
TEST_F(QuantraComparisonTest, Cap_NPVMatches) {
    std::cout << "\n=== Cap ===" << std::endl;
    double notional = 1000000.0, strike = 0.04, vol = 0.20;
    QuantLib::Date start = evaluationDate_ + 2, end = start + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule sch(start, end, QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    QuantLib::Leg leg = QuantLib::IborLeg(sch, idx).withNotionals(notional)
        .withPaymentDayCounter(QuantLib::Actual360()).withPaymentAdjustment(QuantLib::ModifiedFollowing).withFixingDays(2);
    auto qlCap = std::make_shared<QuantLib::Cap>(leg, std::vector<QuantLib::Rate>(1, strike));
    auto volH = QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(
        std::make_shared<QuantLib::ConstantOptionletVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed()));
    qlCap->setPricingEngine(std::make_shared<QuantLib::BlackCapFloorEngine>(discountHandle_, volH));
    double qlNPV = qlCap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto asof = b.CreateString("2025-01-15");
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_curves(curves);
    auto pricing = pb.Finish();
    
    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Quarterly);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto schedule = sb.Finish();
    
    auto idx3m = buildIndex3M(b);
    quantra::CapFloorBuilder cb(b);
    cb.add_cap_floor_type(quantra::CapFloorType_Cap);
    cb.add_notional(notional);
    cb.add_schedule(schedule);
    cb.add_strike(strike);
    cb.add_index(idx3m);
    cb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto cap = cb.Finish();
    
    auto volTS = buildVolatility(b, vol);
    auto dc = b.CreateString("discount");
    
    quantra::PriceCapFloorBuilder pcb(b);
    pcb.add_cap_floor(cap);
    pcb.add_discounting_curve(dc);
    pcb.add_forwarding_curve(dc);
    pcb.add_volatility(volTS);
    auto pcbOff = pcb.Finish();
    
    auto caps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceCapFloor>>{pcbOff});
    
    quantra::PriceCapFloorRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_cap_floors(caps);
    b.Finish(rb.Finish());
    
    CapFloorPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceCapFloorRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceCapFloorResponse>(respB->GetBufferPointer())->cap_floors()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

// ======================== SWAPTION ========================
TEST_F(QuantraComparisonTest, Swaption_NPVMatches) {
    std::cout << "\n=== Swaption ===" << std::endl;
    double notional = 1000000.0, strike = 0.035, vol = 0.20;
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed()));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BlackSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto asof = b.CreateString("2025-01-15");
    
    quantra::PricingBuilder pb(b);
    pb.add_as_of_date(asof);
    pb.add_settlement_date(asof);
    pb.add_curves(curves);
    auto pricing = pb.Finish();
    
    // Fixed leg
    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
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
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();
    
    // Float leg
    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
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
    auto uswap = vsb.Finish();
    
    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_swap(uswap);
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::ExerciseType_European);
    swb.add_settlement_type(quantra::SettlementType_Physical);
    auto swaption = swb.Finish();
    
    auto volTS = buildVolatility(b, vol);
    auto dc = b.CreateString("discount");
    
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(volTS);
    auto psbOff = psb.Finish();
    
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});
    
    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());
    
    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

// ======================== CDS ========================
TEST_F(QuantraComparisonTest, CDS_NPVMatches) {
    std::cout << "\n=== CDS ===" << std::endl;
    double notional = 10000000.0, spread = 0.01, recovery = 0.40, hazard = 0.02;
    QuantLib::Date start = evaluationDate_, end = start + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule sch(start, end, QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(),
        QuantLib::Following, QuantLib::Unadjusted, QuantLib::DateGeneration::TwentiethIMM, false);
    auto qlCDS = std::make_shared<QuantLib::CreditDefaultSwap>(QuantLib::Protection::Buyer,
        notional, spread, sch, QuantLib::Following, QuantLib::Actual360());
    auto defCurve = std::make_shared<QuantLib::FlatHazardRate>(evaluationDate_, hazard, QuantLib::Actual365Fixed());
    qlCDS->setPricingEngine(std::make_shared<QuantLib::MidPointCdsEngine>(
        QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure>(defCurve), recovery, discountHandle_));
    double qlNPV = qlCDS->NPV();
    double qlFair = qlCDS->fairSpread();

    flatbuffers::grpc::MessageBuilder b;
    
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
    
    CDSPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceCDSRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto r = flatbuffers::GetRoot<quantra::PriceCDSResponse>(respB->GetBufferPointer())->cds_list()->Get(0);
    double qNPV = r->npv();
    double qFair = r->fair_spread();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    std::cout << "QuantLib Fair: " << qlFair*10000 << "bps | Quantra: " << qFair*10000 << "bps" << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFair, qFair, 1e-6);
}

}} // namespace