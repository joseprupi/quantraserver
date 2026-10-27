// Basis Swap parity tests (two floating legs, e.g. 3M vs 6M) on the `SwapLeg`
// wire.
//
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary. Each case builds the FlatBuffers
// request (two flat `SwapLeg`s with IborCoupons on `leg1` and `leg2`, each
// projected on its own forwarding curve), prices it through the BasisSwap handler, and compares
// against an independently-built QuantLib::Swap of two IborLegs (the exact
// shape the shared leg builder constructs).
#include "parity_fixture.h"

namespace quantra { namespace testing {

namespace {

constexpr double kBasisPoint = 1.0e-4;

QuantLib::Leg makeIborLeg(
    const QuantLib::Schedule& sch,
    const std::shared_ptr<QuantLib::IborIndex>& idx,
    double notional,
    double spread) {
    return QuantLib::IborLeg(sch, idx)
        .withNotionals(notional)
        .withPaymentDayCounter(QuantLib::Actual360())
        .withPaymentAdjustment(QuantLib::ModifiedFollowing)
        .withSpreads(spread)
        .withFixingDays(2)
        .inArrears(false);
}

flatbuffers::Offset<quantra::Schedule> buildSchedule(
    flatbuffers::grpc::MessageBuilder& b,
    const char* eff,
    const char* term,
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
    sb.add_end_of_month(false);
    return sb.Finish();
}

/// The shared fields of a basis-swap leg: Act/360 coupons on a TARGET
/// schedule from 2025-01-17 to `term`.
SwapLegFields legFields(
    flatbuffers::grpc::MessageBuilder& b,
    const char* term,
    quantra::enums::Frequency freq,
    double notional) {
    return SwapLegFields(
        buildSchedule(b, "2025-01-17", term, freq), notional, quantra::enums::DayCounter_Actual360);
}

/// IBOR coupons on `indexId` plus `spread` (fixing days left to the index: 2).
flatbuffers::Offset<quantra::IborCoupons> buildIborCoupons(
    flatbuffers::grpc::MessageBuilder& b, const char* indexId, double spread) {
    auto idx = quantra::CreateIndexRefDirect(b, indexId);
    quantra::IborCouponsBuilder lb(b);
    lb.add_index(idx);
    lb.add_spread(spread);
    return lb.Finish();
}

} // namespace

class BasisSwapParityTest : public QuantraComparisonTest {
protected:
    /// Prices a one-swap request (EUR_3M + EUR_6M indices, one EUR curve)
    /// through the handler. Each call keeps its own response buffer in
    /// `responses_`, so every returned pointer stays valid until the end of
    /// the test.
    const quantra::BasisSwapResponse* price(
        flatbuffers::grpc::MessageBuilder& b, flatbuffers::Offset<quantra::BasisSwap> swap) {
        auto ts = buildCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        auto indices = buildIndicesVector(b, true); // EUR_3M + EUR_6M
        auto asof = b.CreateString("2025-01-15");
        auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);
        auto entry = quantra::CreatePriceBasisSwap(b, swap);
        auto swaps =
            b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceBasisSwap>>{entry});
        b.Finish(quantra::CreatePriceBasisSwapRequest(b, pricing, swaps));

        BasisSwapPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        responses_.push_back(respB);
        auto resp = req.request(
            respB, flatbuffers::GetRoot<quantra::PriceBasisSwapRequest>(b.GetBufferPointer()));
        respB->Finish(resp);
        return flatbuffers::GetRoot<quantra::PriceBasisSwapResponse>(respB->GetBufferPointer())
            ->swaps()
            ->Get(0);
    }

    std::vector<std::shared_ptr<flatbuffers::grpc::MessageBuilder>> responses_;
};

// Base case: payer basis swap, 3M (leg1, paid) vs 6M (leg2, received), zero
// spreads.
TEST_F(BasisSwapParityTest, BasisSwap_NPVMatches) {
    std::cout << "\n=== Basis Swap ===" << std::endl;
    const double notional = 1000000.0;

    QuantLib::Schedule sch3m(
        QuantLib::Date(17, QuantLib::January, 2025), QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(), QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule sch6m(
        QuantLib::Date(17, QuantLib::January, 2025), QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(), QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx3m = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    auto idx6m = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    std::vector<QuantLib::Leg> legs{
        makeIborLeg(sch3m, idx3m, notional, 0.0), makeIborLeg(sch6m, idx6m, notional, 0.0)};
    std::vector<bool> payer{true, false}; // Payer = pay leg1, receive leg2.
    auto qlSwap = std::make_shared<QuantLib::Swap>(legs, payer);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto swap = buildBasisSwap(
        b, quantra::enums::PayReceive_Pay, "EUR",
        legFields(b, "2030-01-17", quantra::enums::Frequency_Quarterly, notional),
        buildIborCoupons(b, "EUR_3M", 0.0),
        legFields(b, "2030-01-17", quantra::enums::Frequency_Semiannual, notional),
        buildIborCoupons(b, "EUR_6M", 0.0), "discount", "discount", "discount");
    const auto* r = price(b, swap);

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << r->npv()
              << " | Diff: " << std::abs(qlNPV - r->npv()) << std::endl;
    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    // legs[0] = leg1, legs[1] = leg2, signed as QuantLib signs its leg NPVs.
    ASSERT_EQ(r->legs()->size(), 2u);
    EXPECT_NEAR(qlSwap->legNPV(0), r->legs()->Get(0)->npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(1), r->legs()->Get(1)->npv(), 0.01);
    // Neither leg carries a spread: the fair spread is solved on leg2 (the
    // quoted leg by convention), from its signed BPS.
    ASSERT_TRUE(r->fair_spread().has_value());
    EXPECT_NEAR(0.0 - qlNPV * kBasisPoint / qlSwap->legBPS(1), r->fair_spread().value(), 1e-9);
}

// Variation: receiver basis swap with a non-zero spread on leg1 and the tenors
// swapped (6M on leg1, received; 3M on leg2, paid). The fair spread is then
// solved on leg1, the leg carrying the request's spread.
TEST_F(BasisSwapParityTest, BasisSwap_Receiver_SpreadLeg1) {
    const double notional = 2000000.0, spread1 = 0.0015;

    QuantLib::Schedule sch6m(
        QuantLib::Date(17, QuantLib::January, 2025), QuantLib::Date(17, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(), QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule sch3m(
        QuantLib::Date(17, QuantLib::January, 2025), QuantLib::Date(17, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(), QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx6m = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto idx3m = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    std::vector<QuantLib::Leg> legs{
        makeIborLeg(sch6m, idx6m, notional, spread1), makeIborLeg(sch3m, idx3m, notional, 0.0)};
    std::vector<bool> payer{false, true}; // Receiver = receive leg1, pay leg2.
    auto qlSwap = std::make_shared<QuantLib::Swap>(legs, payer);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto swap = buildBasisSwap(
        b, quantra::enums::PayReceive_Receive, "EUR",
        legFields(b, "2028-01-17", quantra::enums::Frequency_Semiannual, notional),
        buildIborCoupons(b, "EUR_6M", spread1),
        legFields(b, "2028-01-17", quantra::enums::Frequency_Quarterly, notional),
        buildIborCoupons(b, "EUR_3M", 0.0), "discount", "discount", "discount");
    const auto* r = price(b, swap);

    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(0), r->legs()->Get(0)->npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(1), r->legs()->Get(1)->npv(), 0.01);
    // The spread on leg1 that zeroes the swap: spread1 - NPV / (leg1 BPS per
    // unit of spread), the BPS carrying the receiver sign.
    ASSERT_TRUE(r->fair_spread().has_value());
    EXPECT_NEAR(spread1 - qlNPV * kBasisPoint / qlSwap->legBPS(0), r->fair_spread().value(), 1e-9);
}

}} // namespace quantra::testing
