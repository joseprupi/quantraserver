// Floating Rate Bond parity tests.
//
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary. Each case builds the FlatBuffers
// request, prices it through the FloatingRateBond handler, and compares against
// an independently-built QuantLib FloatingRateBond (Euribor index forecasting
// off the same curve, BlackIborCouponPricer with the same optionlet vol).
#include "parity_fixture.h"

namespace quantra { namespace testing {

namespace {

// Attach a BlackIborCouponPricer with a ConstantOptionletVolatility, mirroring
// FloatingRateBondEvaluator::buildBlackIborCouponPricer exactly.
void attachIborPricer(const QuantLib::Leg& cashflows, double vol,
                      int settlementDays = 2) {
    auto qlVol = std::make_shared<QuantLib::ConstantOptionletVolatility>(
        settlementDays, QuantLib::TARGET(), QuantLib::ModifiedFollowing, vol,
        QuantLib::Actual365Fixed());
    auto pricer = std::make_shared<QuantLib::BlackIborCouponPricer>();
    pricer->setCapletVolatility(
        QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(qlVol));
    QuantLib::setCouponPricer(cashflows, pricer);
}

} // namespace

// Base case: semiannual 6M-index bond with a positive spread, matching the
// canonical example request. Compares NPV only.
TEST_F(QuantraComparisonTest, FloatingRateBond_NPVMatches) {
    std::cout << "\n=== Floating Rate Bond ===" << std::endl;
    const double face = 100.0, spread = 0.001;

    QuantLib::Schedule sch(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto qlBond = std::make_shared<QuantLib::FloatingRateBond>(
        2, face, sch, idx, QuantLib::Actual360(), QuantLib::ModifiedFollowing, 2,
        std::vector<QuantLib::Real>(1, 1.0), std::vector<QuantLib::Spread>(1, spread),
        std::vector<QuantLib::Rate>(), std::vector<QuantLib::Rate>(), false, 100.0,
        QuantLib::Date(17, QuantLib::January, 2025));
    qlBond->setPricingEngine(std::make_shared<QuantLib::DiscountingBondEngine>(discountHandle_));
    attachIborPricer(qlBond->cashflows(), 0.0);
    const double qlNPV = qlBond->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto pricers = buildCouponPricerVector(b, "iborpricer");
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, pricers);

    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Semiannual);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto schedule = sb.Finish();

    auto idate = b.CreateString("2025-01-17");
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::FloatingRateBondBuilder bb(b);
    bb.add_settlement_days(2);
    bb.add_face_amount(face);
    bb.add_schedule(schedule);
    bb.add_index(idx6m);
    bb.add_accrual_day_counter(quantra::enums::DayCounter_Actual360);
    bb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    bb.add_fixing_days(2);
    bb.add_spread(spread);
    bb.add_in_arrears(false);
    bb.add_redemption(100.0);
    bb.add_issue_date(idate);
    auto bond = bb.Finish();

    auto dc = b.CreateString("discount");
    auto cpId = b.CreateString("iborpricer");
    quantra::PriceFloatingRateBondBuilder pfb(b);
    pfb.add_floating_rate_bond(bond);
    pfb.add_discounting_curve(dc);
    pfb.add_forwarding_curve(dc);
    pfb.add_coupon_pricer(cpId);
    auto pfbOff = pfb.Finish();
    auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFloatingRateBond>>{pfbOff});

    quantra::PriceFloatingRateBondRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_bonds(bonds);
    b.Finish(rb.Finish());

    FloatingRateBondPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFloatingRateBondRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const double qNPV = flatbuffers::GetRoot<quantra::PriceFloatingRateBondResponse>(respB->GetBufferPointer())->bonds()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

// Variation: zero spread, annual frequency, Thirty360 accrual, larger face and
// non-par redemption. Exercises the analytics block too (clean/dirty price,
// accrued) via bondPricingDetails.
TEST_F(QuantraComparisonTest, FloatingRateBond_Annual_Thirty360_Details) {
    const double face = 1000.0, spread = 0.0, redemption = 101.0;

    QuantLib::Schedule sch(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::Following, QuantLib::Following,
        QuantLib::DateGeneration::Backward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto qlBond = std::make_shared<QuantLib::FloatingRateBond>(
        3, face, sch, idx, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis),
        QuantLib::Following, 2,
        std::vector<QuantLib::Real>(1, 1.0), std::vector<QuantLib::Spread>(1, spread),
        std::vector<QuantLib::Rate>(), std::vector<QuantLib::Rate>(), false, redemption,
        QuantLib::Date(17, QuantLib::January, 2025));
    qlBond->setPricingEngine(std::make_shared<QuantLib::DiscountingBondEngine>(discountHandle_));
    attachIborPricer(qlBond->cashflows(), 0.0);
    const double qlNPV = qlBond->NPV();
    const double qlClean = qlBond->cleanPrice();
    const double qlAccrued = qlBond->accruedAmount();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto pricers = buildCouponPricerVector(b, "iborpricer");
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, pricers, 0, 0, 0, 0, 0, 0, true);

    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2028-01-17");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Annual);
    sb.add_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Backward);
    auto schedule = sb.Finish();

    auto idate = b.CreateString("2025-01-17");
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::FloatingRateBondBuilder bb(b);
    bb.add_settlement_days(3);
    bb.add_face_amount(face);
    bb.add_schedule(schedule);
    bb.add_index(idx6m);
    bb.add_accrual_day_counter(quantra::enums::DayCounter_Thirty360);
    bb.add_payment_convention(quantra::enums::BusinessDayConvention_Following);
    bb.add_fixing_days(2);
    bb.add_spread(spread);
    bb.add_in_arrears(false);
    bb.add_redemption(redemption);
    bb.add_issue_date(idate);
    auto bond = bb.Finish();

    auto yield = buildYield(b);
    auto dc = b.CreateString("discount");
    auto cpId = b.CreateString("iborpricer");
    quantra::PriceFloatingRateBondBuilder pfb(b);
    pfb.add_floating_rate_bond(bond);
    pfb.add_discounting_curve(dc);
    pfb.add_forwarding_curve(dc);
    pfb.add_coupon_pricer(cpId);
    pfb.add_yield(yield);
    auto pfbOff = pfb.Finish();
    auto bonds = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFloatingRateBond>>{pfbOff});

    quantra::PriceFloatingRateBondRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_bonds(bonds);
    b.Finish(rb.Finish());

    FloatingRateBondPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFloatingRateBondRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceFloatingRateBondResponse>(respB->GetBufferPointer())->bonds()->Get(0);

    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlClean, r->clean_price(), 1e-4);
    EXPECT_NEAR(qlAccrued, r->accrued_amount(), 1e-4);
}

}} // namespace quantra::testing
