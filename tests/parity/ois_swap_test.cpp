// OIS (Overnight Indexed) Swap parity tests on the `SwapLeg` wire.
//
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary. Each case builds the FlatBuffers
// request (FixedCoupons on `fixed_leg`, OvernightCoupons on `overnight_leg`),
// prices it through the OisSwap handler, and compares against an
// independently-built QuantLib OvernightIndexedSwap (overnight index
// forecasting off the same curve, OIS discounting). QuantLib applies the
// swap's payment lag / payment calendar to BOTH legs, so both legs state
// them on the wire (`SwapLeg.payment_calendar` / `payment_lag`, required on
// price-ois-swap; the coupon tables carry only the rate / the index terms).
#include "date_convert.h"
#include "parity_fixture.h"

#include <functional>

namespace quantra { namespace testing {

namespace {

// Mirror the SOFR overnight index the registry builds from the USD_SOFR
// IndexDef (name "SOFR", 0 fixing days, USD, US GovernmentBond, Actual360).
std::shared_ptr<QuantLib::OvernightIndex> makeSofr(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& fwd) {
    return std::make_shared<QuantLib::OvernightIndex>(
        "SOFR", 0, QuantLib::USDCurrency(),
        QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond), QuantLib::Actual360(), fwd);
}

flatbuffers::Offset<quantra::Schedule> fbSchedule(
    flatbuffers::grpc::MessageBuilder& b,
    const char* term,
    quantra::enums::Frequency freq,
    quantra::enums::Calendar calendar) {
    auto eff = b.CreateString("2025-01-17");
    auto termOff = b.CreateString(term);
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(termOff);
    sb.add_calendar(calendar);
    sb.add_frequency(freq);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    sb.add_end_of_month(false);
    return sb.Finish();
}

/// The shared fields of the fixed leg: Act/360 coupons on a TARGET schedule,
/// paid on `paymentCalendar` with `paymentLag` business days of lag.
/// QuantLib's OvernightIndexedSwap applies its payment calendar and lag to
/// the fixed leg too, so the twin of `qlSwap()` pays on the US
/// GovernmentBond calendar; `price-ois-swap` requires both fields on both
/// legs (absent is a 400, not "the schedule's calendar and no lag").
SwapLegFields fixedFields(
    flatbuffers::grpc::MessageBuilder& b,
    const char* term,
    quantra::enums::Frequency freq,
    double notional,
    quantra::enums::Calendar paymentCalendar = quantra::enums::Calendar_UnitedStatesGovernmentBond,
    int paymentLag = 0) {
    return SwapLegFields(
               fbSchedule(b, term, freq, quantra::enums::Calendar_TARGET), notional,
               quantra::enums::DayCounter_Actual360)
        .withPaymentTerms(paymentCalendar, paymentLag);
}

/// The shared fields of the overnight leg: a US GovernmentBond schedule paid
/// on the same calendar with `paymentLag` (the day counter is the index's
/// own; Act/360 is stated for the wire but not read).
SwapLegFields overnightFields(
    flatbuffers::grpc::MessageBuilder& b,
    const char* term,
    quantra::enums::Frequency freq,
    double notional,
    int paymentLag) {
    return SwapLegFields(
               fbSchedule(b, term, freq, quantra::enums::Calendar_UnitedStatesGovernmentBond),
               notional, quantra::enums::DayCounter_Actual360)
        .withPaymentTerms(quantra::enums::Calendar_UnitedStatesGovernmentBond, paymentLag);
}

/// SOFR coupons compounded over each period, plus `spread`.
flatbuffers::Offset<quantra::OvernightCoupons> fbOvernight(
    flatbuffers::grpc::MessageBuilder& b, double spread) {
    auto sofrRef = b.CreateString("USD_SOFR");
    auto sofr = quantra::CreateIndexRef(b, sofrRef);
    quantra::OvernightCouponsBuilder ob(b);
    ob.add_index(sofr);
    ob.add_spread(spread);
    ob.add_averaging_method(quantra::enums::RateAveragingType_Compound);
    ob.add_lookback_days(0);
    ob.add_lockout_days(0);
    ob.add_apply_observation_shift(false);
    return ob.Finish();
}

/// A USD OIS on the one curve: FixedCoupons at `fixedRate` on `fixed`,
/// SOFR coupons plus `spread` on `overnight`, the fixed leg on side
/// `fixedSide`.
flatbuffers::Offset<quantra::OisSwap> fbSwap(
    flatbuffers::grpc::MessageBuilder& b,
    quantra::enums::PayReceive fixedSide,
    const SwapLegFields& fixed,
    double fixedRate,
    const SwapLegFields& overnight,
    double spread) {
    return buildOisSwap(
        b, fixedSide, "USD", fixed, quantra::CreateFixedCoupons(b, fixedRate), overnight,
        fbOvernight(b, spread), "discount", "discount");
}

} // namespace

class OisSwapParityTest : public QuantraComparisonTest {
protected:
    /// Prices a one-swap request through the handler; the response buffer
    /// stays alive in `respB_` until the next call.
    const quantra::OisSwapResponse* price(
        flatbuffers::grpc::MessageBuilder& b,
        flatbuffers::Offset<quantra::OisSwap> swap,
        bool includeFlows = false) {
        auto ts = buildCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        auto indices = buildIndicesVectorWithSofr(b);
        auto asof = b.CreateString("2025-01-15");
        auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);
        auto entry = quantra::CreatePriceOisSwap(b, swap);
        auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceOisSwap>>{entry});
        b.Finish(quantra::CreatePriceOisSwapRequest(b, pricing, swaps, includeFlows));

        OisSwapPricingRequest req;
        respB_ = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(
            respB_, flatbuffers::GetRoot<quantra::PriceOisSwapRequest>(b.GetBufferPointer()));
        respB_->Finish(resp);
        return flatbuffers::GetRoot<quantra::PriceOisSwapResponse>(respB_->GetBufferPointer())
            ->swaps()
            ->Get(0);
    }

    /// The QuantLib twin: schedules from 2025-01-17 to `end`, fixed leg on
    /// TARGET, overnight leg on US GovernmentBond, payment lag on both legs.
    std::shared_ptr<QuantLib::OvernightIndexedSwap> qlSwap(
        QuantLib::OvernightIndexedSwap::Type type,
        const QuantLib::Date& end,
        QuantLib::Frequency freq,
        double notional,
        double fixedRate,
        double spread,
        int paymentLag) {
        const QuantLib::Date start(17, QuantLib::January, 2025);
        QuantLib::Schedule fixedSch(
            start, end, QuantLib::Period(freq), QuantLib::TARGET(), QuantLib::ModifiedFollowing,
            QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
        QuantLib::Schedule onSch(
            start, end, QuantLib::Period(freq),
            QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
            QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
            QuantLib::DateGeneration::Forward, false);
        auto swap = std::make_shared<QuantLib::OvernightIndexedSwap>(
            type, notional, fixedSch, fixedRate, QuantLib::Actual360(), onSch,
            makeSofr(forwardHandle_), spread, paymentLag, QuantLib::ModifiedFollowing,
            QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond), false,
            QuantLib::RateAveraging::Compound, QuantLib::Null<QuantLib::Natural>(), 0, false);
        swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
        return swap;
    }

    std::string errorOf(const std::function<void()>& fn) {
        try {
            fn();
        } catch (const QuantraError& e) {
            return e.what();
        }
        return "";
    }

    std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB_;
};

// Base case: payer OIS, annual schedules, compounded averaging, zero spread.
TEST_F(OisSwapParityTest, OisSwap_NPVMatches) {
    std::cout << "\n=== OIS Swap ===" << std::endl;
    const double notional = 1000000.0, fixedRate = 0.03;
    auto ql = qlSwap(
        QuantLib::OvernightIndexedSwap::Payer, QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Annual, notional, fixedRate, 0.0, 0);
    const double qlNPV = ql->NPV();
    const double qlFair = ql->fairRate();

    flatbuffers::grpc::MessageBuilder b;
    // Payer: the fixed leg is paid, SOFR received; both legs in USD on the
    // one curve, which also projects the overnight leg.
    auto swap = fbSwap(
        b, quantra::enums::PayReceive_Pay,
        fixedFields(b, "2030-01-17", quantra::enums::Frequency_Annual, notional), fixedRate,
        overnightFields(b, "2030-01-17", quantra::enums::Frequency_Annual, notional, 0), 0.0);
    const auto* r = price(b, swap);

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << r->npv()
              << " | Diff: " << std::abs(qlNPV - r->npv()) << std::endl;
    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    ASSERT_TRUE(r->fair_rate().has_value());
    EXPECT_NEAR(qlFair, r->fair_rate().value(), 1e-6);
    // legs[0] = fixed_leg, legs[1] = overnight_leg, signed as QuantLib signs
    // its leg NPVs (paid fixed leg negative).
    ASSERT_EQ(r->legs()->size(), 2u);
    EXPECT_NEAR(ql->fixedLegNPV(), r->legs()->Get(0)->npv(), 0.01);
    EXPECT_NEAR(ql->overnightLegNPV(), r->legs()->Get(1)->npv(), 0.01);
    EXPECT_NEAR(r->npv(), r->legs()->Get(0)->npv() + r->legs()->Get(1)->npv(), 1e-9);
}

// fair_spread is QuantLib::OvernightIndexedSwap::fairSpread (the spread on
// the overnight leg that zeroes the swap, the fixed leg held fixed), and the
// OIS repriced through the API with that spread has a zero NPV.
TEST_F(OisSwapParityTest, OisSwap_FairSpreadMatchesQuantLibAndReprices) {
    const double notional = 1000000.0, fixedRate = 0.03;
    auto ql = qlSwap(
        QuantLib::OvernightIndexedSwap::Payer, QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Annual, notional, fixedRate, 0.0, 0);
    const double qlFairSpread = ql->fairSpread();

    flatbuffers::grpc::MessageBuilder b;
    auto swap = fbSwap(
        b, quantra::enums::PayReceive_Pay,
        fixedFields(b, "2030-01-17", quantra::enums::Frequency_Annual, notional), fixedRate,
        overnightFields(b, "2030-01-17", quantra::enums::Frequency_Annual, notional, 0), 0.0);
    const auto* r = price(b, swap);
    ASSERT_TRUE(r->fair_spread().has_value());
    EXPECT_NEAR(qlFairSpread, r->fair_spread().value(), 1e-12);
    const double fairSpread = r->fair_spread().value();

    flatbuffers::grpc::MessageBuilder b2;
    auto atFair = fbSwap(
        b2, quantra::enums::PayReceive_Pay,
        fixedFields(b2, "2030-01-17", quantra::enums::Frequency_Annual, notional), fixedRate,
        overnightFields(b2, "2030-01-17", quantra::enums::Frequency_Annual, notional, 0),
        fairSpread);
    const auto* r2 = price(b2, atFair);
    EXPECT_LT(std::abs(r2->npv()), 1e-9 * notional);
    ASSERT_TRUE(r2->fair_spread().has_value());
    EXPECT_NEAR(fairSpread, r2->fair_spread().value(), 1e-12);
    ASSERT_TRUE(r2->fair_rate().has_value());
}

// Variation: receiver OIS, semiannual schedules, non-zero overnight spread and a
// 2-day payment lag (on both legs, as QuantLib applies it). Exercises a
// different leg shape than the base case.
TEST_F(OisSwapParityTest, OisSwap_Receiver_Semiannual_SpreadLag) {
    const double notional = 5000000.0, fixedRate = 0.028, spread = 0.001;
    const int paymentLag = 2;
    auto ql = qlSwap(
        QuantLib::OvernightIndexedSwap::Receiver, QuantLib::Date(17, QuantLib::January, 2028),
        QuantLib::Semiannual, notional, fixedRate, spread, paymentLag);

    flatbuffers::grpc::MessageBuilder b;
    // Receiver: the fixed leg is received, SOFR (plus spread) paid.
    auto swap = fbSwap(
        b, quantra::enums::PayReceive_Receive,
        fixedFields(
            b, "2028-01-17", quantra::enums::Frequency_Semiannual, notional,
            quantra::enums::Calendar_UnitedStatesGovernmentBond, paymentLag),
        fixedRate,
        overnightFields(
            b, "2028-01-17", quantra::enums::Frequency_Semiannual, notional, paymentLag),
        spread);
    const auto* r = price(b, swap);

    EXPECT_NEAR(ql->NPV(), r->npv(), 0.01);
    ASSERT_TRUE(r->fair_rate().has_value());
    EXPECT_NEAR(ql->fairRate(), r->fair_rate().value(), 1e-6);
    ASSERT_TRUE(r->fair_spread().has_value());
    EXPECT_NEAR(ql->fairSpread(), r->fair_spread().value(), 1e-12);
    EXPECT_NEAR(ql->fixedLegNPV(), r->legs()->Get(0)->npv(), 0.01);
    EXPECT_NEAR(ql->overnightLegNPV(), r->legs()->Get(1)->npv(), 0.01);
}

// The fixed leg's payment_lag / payment_calendar reproduce QuantLib's
// OvernightIndexedSwap(... paymentLag = 2 ...), which lags the fixed coupons
// too; every fixed payment lands two US business days after its accrual end
// (the JSON twin is examples/data/ir_swaps/ois_eur_5y_payer_estr_payment_lag2.json).
TEST_F(OisSwapParityTest, OisSwap_FixedLegPaymentLag_MatchesQuantLib) {
    const double notional = 10000000.0, fixedRate = 0.0268;
    const int paymentLag = 2;
    auto ql = qlSwap(
        QuantLib::OvernightIndexedSwap::Payer, QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Annual, notional, fixedRate, 0.0, paymentLag);

    flatbuffers::grpc::MessageBuilder b;
    auto swap = fbSwap(
        b, quantra::enums::PayReceive_Pay,
        fixedFields(
            b, "2030-01-17", quantra::enums::Frequency_Annual, notional,
            quantra::enums::Calendar_UnitedStatesGovernmentBond, paymentLag),
        fixedRate,
        overnightFields(b, "2030-01-17", quantra::enums::Frequency_Annual, notional, paymentLag),
        0.0);
    const auto* r = price(b, swap, true);

    EXPECT_NEAR(ql->NPV(), r->npv(), 0.01);
    ASSERT_TRUE(r->fair_rate().has_value());
    EXPECT_NEAR(ql->fairRate(), r->fair_rate().value(), 1e-6);
    EXPECT_NEAR(ql->fixedLegNPV(), r->legs()->Get(0)->npv(), 0.01);
    EXPECT_NEAR(ql->overnightLegNPV(), r->legs()->Get(1)->npv(), 0.01);

    // The fixed flows pay on QuantLib's lagged dates, not on the accrual ends.
    const auto* fixedFlows = r->legs()->Get(0)->flows();
    ASSERT_TRUE(fixedFlows);
    ASSERT_EQ(fixedFlows->size(), ql->fixedLeg().size());
    const QuantLib::Calendar usGov(QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond));
    for (unsigned i = 0; i < fixedFlows->size(); ++i) {
        const auto* f = fixedFlows->Get(i);
        const QuantLib::Date accrualEnd = DateToQL(f->accrual_end_date()->str());
        const QuantLib::Date expected = usGov.advance(accrualEnd, paymentLag, QuantLib::Days);
        EXPECT_EQ(f->payment_date()->str(), DateToIso(expected)) << "flow " << i;
        EXPECT_EQ(f->payment_date()->str(), DateToIso(ql->fixedLeg()[i]->date())) << "flow " << i;
    }
}

// With the fixed leg paying on its own TARGET schedule dates (payment
// calendar TARGET, lag 0) the swap is a different instrument: the fixed
// coupons pay two US business days earlier and the NPV moves (the fields
// are not cosmetic).
TEST_F(OisSwapParityTest, OisSwap_FixedLegPaymentLag_ChangesTheNpv) {
    const double notional = 10000000.0, fixedRate = 0.0268;
    flatbuffers::grpc::MessageBuilder b1, b2;
    const double lagged =
        price(
            b1,
            fbSwap(
                b1, quantra::enums::PayReceive_Pay,
                fixedFields(
                    b1, "2030-01-17", quantra::enums::Frequency_Annual, notional,
                    quantra::enums::Calendar_UnitedStatesGovernmentBond, 2),
                fixedRate,
                overnightFields(b1, "2030-01-17", quantra::enums::Frequency_Annual, notional, 2),
                0.0))
            ->npv();
    const double unlagged =
        price(
            b2,
            fbSwap(
                b2, quantra::enums::PayReceive_Pay,
                fixedFields(
                    b2, "2030-01-17", quantra::enums::Frequency_Annual, notional,
                    quantra::enums::Calendar_TARGET, 0),
                fixedRate,
                overnightFields(b2, "2030-01-17", quantra::enums::Frequency_Annual, notional, 2),
                0.0))
            ->npv();
    EXPECT_GT(std::abs(lagged - unlagged), 1.0);
}

// price-ois-swap requires the payment calendar and lag on both legs: an OIS
// pays with the overnight calendar and lag, and leaving the fixed leg's lag
// out is a 400 naming the leg-level field, not a silent zero.
TEST_F(OisSwapParityTest, OisSwap_FixedLegPaymentLagIsRequired) {
    const double notional = 10000000.0, fixedRate = 0.0268;
    flatbuffers::grpc::MessageBuilder b;
    SwapLegFields fixed = fixedFields(b, "2030-01-17", quantra::enums::Frequency_Annual, notional);
    fixed.paymentLag.reset();
    auto swap = fbSwap(
        b, quantra::enums::PayReceive_Pay, fixed, fixedRate,
        overnightFields(b, "2030-01-17", quantra::enums::Frequency_Annual, notional, 2), 0.0);
    const std::string msg = errorOf([&] { price(b, swap); });
    EXPECT_NE(msg.find("OisSwap.fixed_leg.payment_lag is required"), std::string::npos) << msg;
}

}} // namespace quantra::testing
