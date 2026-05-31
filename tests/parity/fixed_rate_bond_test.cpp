// Fixed Rate Bond parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

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
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    // IndexDefs needed by SwapHelpers in the curve
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, 0, 0, 0, 0, 0, 0, 0, true);
    
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

// Semiannual Thirty360 bond, ModifiedFollowing schedule + payment, 3 settlement
// days, non-default issue date. Compares NPV plus the analytics block
// (clean/dirty price, accrued) so the details path is exercised. Mirrors the
// FixedRateBond ctor the parser uses (rate/dc/payment_convention/redemption/issue).
TEST_F(QuantraComparisonTest, FixedRateBond_Semiannual_Thirty360_Details) {
    const double face = 100.0, coupon = 0.04;
    QuantLib::Schedule sch(
        QuantLib::Date(15, QuantLib::July, 2024),
        QuantLib::Date(15, QuantLib::July, 2029),
        QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Backward, false);
    auto qlBond = std::make_shared<QuantLib::FixedRateBond>(
        3, face, sch, std::vector<QuantLib::Rate>(1, coupon),
        QuantLib::Thirty360(QuantLib::Thirty360::BondBasis),
        QuantLib::ModifiedFollowing, 100.0,
        QuantLib::Date(15, QuantLib::July, 2024));
    qlBond->setPricingEngine(std::make_shared<QuantLib::DiscountingBondEngine>(discountHandle_));
    const double qlNPV = qlBond->NPV();
    const double qlClean = qlBond->cleanPrice();
    const double qlAccrued = qlBond->accruedAmount();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, 0, 0, 0, 0, 0, 0, 0, true);

    auto eff = b.CreateString("2024-07-15");
    auto term = b.CreateString("2029-07-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Semiannual);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
    auto schedule = sb.Finish();

    auto idate = b.CreateString("2024-07-15");
    quantra::FixedRateBondBuilder bb(b);
    bb.add_settlement_days(3);
    bb.add_face_amount(face);
    bb.add_schedule(schedule);
    bb.add_rate(coupon);
    bb.add_accrual_day_counter(quantra::enums::DayCounter_Thirty360);
    bb.add_issue_date(idate);
    bb.add_redemption(100.0);
    bb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto bond = bb.Finish();

    auto yield = buildYield(b);
    auto dc = b.CreateString("discount");
    quantra::PriceFixedRateBondBuilder pfb(b);
    pfb.add_fixed_rate_bond(bond);
    pfb.add_discounting_curve(dc);
    pfb.add_yield(yield);
    auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>>{pfb.Finish()});

    quantra::PriceFixedRateBondRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_bonds(bonds);
    b.Finish(rb.Finish());

    FixedRateBondPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFixedRateBondRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceFixedRateBondResponse>(respB->GetBufferPointer())->bonds()->Get(0);

    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlClean, r->clean_price(), 1e-4);
    EXPECT_NEAR(qlAccrued, r->accrued_amount(), 1e-4);
}

// Quarterly ActualActual bond with Following convention and a non-par
// redemption (102). Different frequency/redemption from the other cases.
TEST_F(QuantraComparisonTest, FixedRateBond_Quarterly_NonParRedemption) {
    const double face = 100.0, coupon = 0.045, redemption = 102.0;
    QuantLib::Schedule sch(
        QuantLib::Date(15, QuantLib::January, 2025),
        QuantLib::Date(15, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(),
        QuantLib::Following, QuantLib::Following,
        QuantLib::DateGeneration::Forward, false);
    auto qlBond = std::make_shared<QuantLib::FixedRateBond>(
        2, face, sch, std::vector<QuantLib::Rate>(1, coupon),
        QuantLib::ActualActual(QuantLib::ActualActual::ISDA),
        QuantLib::Following, redemption,
        QuantLib::Date(15, QuantLib::January, 2025));
    qlBond->setPricingEngine(std::make_shared<QuantLib::DiscountingBondEngine>(discountHandle_));
    const double qlNPV = qlBond->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);

    auto eff = b.CreateString("2025-01-15");
    auto term = b.CreateString("2028-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Quarterly);
    sb.add_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto schedule = sb.Finish();

    auto idate = b.CreateString("2025-01-15");
    quantra::FixedRateBondBuilder bb(b);
    bb.add_settlement_days(2);
    bb.add_face_amount(face);
    bb.add_schedule(schedule);
    bb.add_rate(coupon);
    bb.add_accrual_day_counter(quantra::enums::DayCounter_ActualActual);
    bb.add_issue_date(idate);
    bb.add_redemption(redemption);
    bb.add_payment_convention(quantra::enums::BusinessDayConvention_Following);
    auto bond = bb.Finish();

    auto yield = buildYield(b);
    auto dc = b.CreateString("discount");
    quantra::PriceFixedRateBondBuilder pfb(b);
    pfb.add_fixed_rate_bond(bond);
    pfb.add_discounting_curve(dc);
    pfb.add_yield(yield);
    auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFixedRateBond>>{pfb.Finish()});

    quantra::PriceFixedRateBondRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_bonds(bonds);
    b.Finish(rb.Finish());

    FixedRateBondPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFixedRateBondRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const double qNPV = flatbuffers::GetRoot<quantra::PriceFixedRateBondResponse>(respB->GetBufferPointer())->bonds()->Get(0)->npv();

    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

}} // namespace quantra::testing
