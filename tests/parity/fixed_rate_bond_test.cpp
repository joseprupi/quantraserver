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

}} // namespace quantra::testing
