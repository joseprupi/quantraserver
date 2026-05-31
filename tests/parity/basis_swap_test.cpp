// Basis Swap parity tests (two floating legs, e.g. 3M vs 6M).
//
// New in refactor step 6e (fill zero/thin parity gaps). Shares
// QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary. Each case builds the FlatBuffers
// request, prices it through the BasisSwap handler, and compares against an
// independently-built QuantLib::Swap of two IborLegs (the exact shape the
// BasisSwap pricer constructs).
#include "parity_fixture.h"

namespace quantra { namespace testing {

namespace {

QuantLib::Leg makeIborLeg(const QuantLib::Schedule& sch,
                          const std::shared_ptr<QuantLib::IborIndex>& idx,
                          double notional, double spread) {
    return QuantLib::IborLeg(sch, idx)
        .withNotionals(notional)
        .withPaymentDayCounter(QuantLib::Actual360())
        .withPaymentAdjustment(QuantLib::ModifiedFollowing)
        .withSpreads(spread)
        .withFixingDays(2)
        .inArrears(false);
}

flatbuffers::Offset<quantra::Schedule> buildSchedule(
    flatbuffers::grpc::MessageBuilder& b, const char* eff, const char* term,
    quantra::enums::Frequency freq) {
    auto e = b.CreateString(eff);
    auto t = b.CreateString(term);
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(e);
    sb.add_termination_date(t);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(freq);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    return sb.Finish();
}

} // namespace

// Base case: payer basis swap, 3M (leg1) vs 6M (leg2), zero spreads.
TEST_F(QuantraComparisonTest, BasisSwap_NPVMatches) {
    std::cout << "\n=== Basis Swap ===" << std::endl;
    const double notional = 1000000.0;

    QuantLib::Schedule sch3m(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule sch6m(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    auto idx3m = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    auto idx6m = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    std::vector<QuantLib::Leg> legs{
        makeIborLeg(sch3m, idx3m, notional, 0.0),
        makeIborLeg(sch6m, idx6m, notional, 0.0)};
    std::vector<bool> payer{true, false};  // Payer = pay leg1, receive leg2.
    auto qlSwap = std::make_shared<QuantLib::Swap>(legs, payer);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b, true);  // EUR_3M + EUR_6M
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);

    auto sch1 = buildSchedule(b, "2025-01-17", "2030-01-17", quantra::enums::Frequency_Quarterly);
    auto leg1Idx = buildIndexRef(b, "EUR_3M");
    quantra::SwapFloatingLegBuilder leg1b(b);
    leg1b.add_notional(notional);
    leg1b.add_schedule(sch1);
    leg1b.add_index(leg1Idx);
    leg1b.add_spread(0.0);
    leg1b.add_day_counter(quantra::enums::DayCounter_Actual360);
    leg1b.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto leg1 = leg1b.Finish();

    auto sch2 = buildSchedule(b, "2025-01-17", "2030-01-17", quantra::enums::Frequency_Semiannual);
    auto leg2Idx = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder leg2b(b);
    leg2b.add_notional(notional);
    leg2b.add_schedule(sch2);
    leg2b.add_index(leg2Idx);
    leg2b.add_spread(0.0);
    leg2b.add_day_counter(quantra::enums::DayCounter_Actual360);
    leg2b.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto leg2 = leg2b.Finish();

    quantra::BasisSwapBuilder bsb(b);
    bsb.add_swap_type(quantra::enums::SwapType_Payer);
    bsb.add_leg1(leg1);
    bsb.add_leg2(leg2);
    auto basisSwap = bsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceBasisSwapBuilder psb(b);
    psb.add_basis_swap(basisSwap);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve_leg1(dc);
    psb.add_forwarding_curve_leg2(dc);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceBasisSwap>>{psb.Finish()});

    quantra::PriceBasisSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    BasisSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceBasisSwapRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceBasisSwapResponse>(respB->GetBufferPointer())->swaps()->Get(0);

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << r->npv() << " | Diff: " << std::abs(qlNPV-r->npv()) << std::endl;
    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(0), r->leg1_npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(1), r->leg2_npv(), 0.01);
}

// Variation: receiver basis swap with a non-zero spread on leg1 and the tenors
// swapped (6M on leg1, 3M on leg2). Also checks the per-leg BPS outputs.
TEST_F(QuantraComparisonTest, BasisSwap_Receiver_SpreadLeg1) {
    const double notional = 2000000.0, spread1 = 0.0015;

    QuantLib::Schedule sch6m(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule sch3m(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    auto idx6m = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto idx3m = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    std::vector<QuantLib::Leg> legs{
        makeIborLeg(sch6m, idx6m, notional, spread1),
        makeIborLeg(sch3m, idx3m, notional, 0.0)};
    std::vector<bool> payer{false, true};  // Receiver = receive leg1, pay leg2.
    auto qlSwap = std::make_shared<QuantLib::Swap>(legs, payer);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b, true);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);

    auto sch1 = buildSchedule(b, "2025-01-17", "2028-01-17", quantra::enums::Frequency_Semiannual);
    auto leg1Idx = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder leg1b(b);
    leg1b.add_notional(notional);
    leg1b.add_schedule(sch1);
    leg1b.add_index(leg1Idx);
    leg1b.add_spread(spread1);
    leg1b.add_day_counter(quantra::enums::DayCounter_Actual360);
    leg1b.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto leg1 = leg1b.Finish();

    auto sch2 = buildSchedule(b, "2025-01-17", "2028-01-17", quantra::enums::Frequency_Quarterly);
    auto leg2Idx = buildIndexRef(b, "EUR_3M");
    quantra::SwapFloatingLegBuilder leg2b(b);
    leg2b.add_notional(notional);
    leg2b.add_schedule(sch2);
    leg2b.add_index(leg2Idx);
    leg2b.add_spread(0.0);
    leg2b.add_day_counter(quantra::enums::DayCounter_Actual360);
    leg2b.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto leg2 = leg2b.Finish();

    quantra::BasisSwapBuilder bsb(b);
    bsb.add_swap_type(quantra::enums::SwapType_Receiver);
    bsb.add_leg1(leg1);
    bsb.add_leg2(leg2);
    auto basisSwap = bsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceBasisSwapBuilder psb(b);
    psb.add_basis_swap(basisSwap);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve_leg1(dc);
    psb.add_forwarding_curve_leg2(dc);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceBasisSwap>>{psb.Finish()});

    quantra::PriceBasisSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    BasisSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceBasisSwapRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceBasisSwapResponse>(respB->GetBufferPointer())->swaps()->Get(0);

    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(0), r->leg1_npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(1), r->leg2_npv(), 0.01);
    EXPECT_NEAR(qlSwap->legBPS(0), r->leg1_bps(), 1e-2);
    EXPECT_NEAR(qlSwap->legBPS(1), r->leg2_bps(), 1e-2);
}

}} // namespace quantra::testing
