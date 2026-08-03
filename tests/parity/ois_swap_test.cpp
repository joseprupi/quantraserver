// OIS (Overnight Indexed) Swap parity tests.
//
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary. Each case builds the FlatBuffers
// request, prices it through the OisSwap handler, and compares against an
// independently-built QuantLib OvernightIndexedSwap (overnight index forecasting
// off the same curve, OIS discounting).
#include "parity_fixture.h"

namespace quantra { namespace testing {

namespace {

// Mirror the SOFR overnight index the registry builds from the USD_SOFR
// IndexDef (name "SOFR", 0 fixing days, USD, US GovernmentBond, Actual360).
std::shared_ptr<QuantLib::OvernightIndex> makeSofr(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& fwd) {
    return std::make_shared<QuantLib::OvernightIndex>(
        "SOFR", 0, QuantLib::USDCurrency(),
        QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
        QuantLib::Actual360(), fwd);
}

} // namespace

// Base case: payer OIS, annual schedules, compounded averaging, zero spread.
TEST_F(QuantraComparisonTest, OisSwap_NPVMatches) {
    std::cout << "\n=== OIS Swap ===" << std::endl;
    const double notional = 1000000.0, fixedRate = 0.03;

    QuantLib::Schedule fixedSch(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule onSch(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(QuantLib::Annual),
        QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    auto sofr = makeSofr(forwardHandle_);
    auto qlSwap = std::make_shared<QuantLib::OvernightIndexedSwap>(
        QuantLib::OvernightIndexedSwap::Payer, notional, fixedSch, fixedRate,
        QuantLib::Actual360(), onSch, sofr, 0.0, 0,
        QuantLib::ModifiedFollowing,
        QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond), false,
        QuantLib::RateAveraging::Compound,
        QuantLib::Null<QuantLib::Natural>(), 0, false);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();
    const double qlFair = qlSwap->fairRate();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVectorWithSofr(b);
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
    fsb.add_end_of_month(false);
    auto fixedSchOff = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSchOff);
    flb.add_rate(fixedRate);
    flb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto oeff = b.CreateString("2025-01-17");
    auto oterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder osb(b);
    osb.add_effective_date(oeff);
    osb.add_termination_date(oterm);
    osb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    osb.add_frequency(quantra::enums::Frequency_Annual);
    osb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    osb.add_end_of_month(false);
    auto onSchOff = osb.Finish();

    auto sofrRef = buildIndexRef(b, "USD_SOFR");
    quantra::OisFloatingLegBuilder ob(b);
    ob.add_notional(notional);
    ob.add_schedule(onSchOff);
    ob.add_index(sofrRef);
    ob.add_spread(0.0);
    ob.add_day_counter(quantra::enums::DayCounter_Actual360);
    ob.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    ob.add_payment_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    ob.add_payment_lag(0);
    ob.add_averaging_method(quantra::enums::RateAveragingType_Compound);
    ob.add_lookback_days(0);
    ob.add_lockout_days(0);
    ob.add_apply_observation_shift(false);
    auto onLeg = ob.Finish();

    quantra::OisSwapBuilder sb(b);
    sb.add_swap_type(quantra::enums::SwapType_Payer);
    sb.add_fixed_leg(fixedLeg);
    sb.add_overnight_leg(onLeg);
    auto swap = sb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceOisSwapBuilder psb(b);
    psb.add_ois_swap(swap);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceOisSwap>>{psb.Finish()});

    quantra::PriceOisSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    OisSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceOisSwapRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceOisSwapResponse>(respB->GetBufferPointer())->swaps()->Get(0);

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << r->npv() << " | Diff: " << std::abs(qlNPV-r->npv()) << std::endl;
    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlFair, r->fair_rate(), 1e-6);
    EXPECT_NEAR(qlSwap->fixedLegNPV(), r->fixed_leg_npv(), 0.01);
    EXPECT_NEAR(qlSwap->overnightLegNPV(), r->overnight_leg_npv(), 0.01);
}

// Variation: receiver OIS, semiannual schedules, non-zero overnight spread and a
// 2-day payment lag. Exercises a different leg shape than the base case.
TEST_F(QuantraComparisonTest, OisSwap_Receiver_Semiannual_SpreadLag) {
    const double notional = 5000000.0, fixedRate = 0.028, spread = 0.001;
    const int paymentLag = 2;

    QuantLib::Schedule fixedSch(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule onSch(
        QuantLib::Date(17, QuantLib::January, 2025),
        QuantLib::Date(17, QuantLib::January, 2028),
        QuantLib::Period(QuantLib::Semiannual),
        QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    auto sofr = makeSofr(forwardHandle_);
    auto qlSwap = std::make_shared<QuantLib::OvernightIndexedSwap>(
        QuantLib::OvernightIndexedSwap::Receiver, notional, fixedSch, fixedRate,
        QuantLib::Actual360(), onSch, sofr, spread, paymentLag,
        QuantLib::ModifiedFollowing,
        QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond), false,
        QuantLib::RateAveraging::Compound,
        QuantLib::Null<QuantLib::Natural>(), 0, false);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();
    const double qlFairSpread = qlSwap->fairSpread();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVectorWithSofr(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);

    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2028-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(eff);
    fsb.add_termination_date(term);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Semiannual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSchOff = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSchOff);
    flb.add_rate(fixedRate);
    flb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto oeff = b.CreateString("2025-01-17");
    auto oterm = b.CreateString("2028-01-17");
    quantra::ScheduleBuilder osb(b);
    osb.add_effective_date(oeff);
    osb.add_termination_date(oterm);
    osb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    osb.add_frequency(quantra::enums::Frequency_Semiannual);
    osb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    osb.add_end_of_month(false);
    auto onSchOff = osb.Finish();

    auto sofrRef = buildIndexRef(b, "USD_SOFR");
    quantra::OisFloatingLegBuilder ob(b);
    ob.add_notional(notional);
    ob.add_schedule(onSchOff);
    ob.add_index(sofrRef);
    ob.add_spread(spread);
    ob.add_day_counter(quantra::enums::DayCounter_Actual360);
    ob.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    ob.add_payment_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    ob.add_payment_lag(paymentLag);
    ob.add_averaging_method(quantra::enums::RateAveragingType_Compound);
    ob.add_lookback_days(0);
    ob.add_lockout_days(0);
    ob.add_apply_observation_shift(false);
    auto onLeg = ob.Finish();

    quantra::OisSwapBuilder sb(b);
    sb.add_swap_type(quantra::enums::SwapType_Receiver);
    sb.add_fixed_leg(fixedLeg);
    sb.add_overnight_leg(onLeg);
    auto swap = sb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceOisSwapBuilder psb(b);
    psb.add_ois_swap(swap);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceOisSwap>>{psb.Finish()});

    quantra::PriceOisSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    OisSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceOisSwapRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceOisSwapResponse>(respB->GetBufferPointer())->swaps()->Get(0);

    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlFairSpread, r->fair_spread(), 1e-6);
}

}} // namespace quantra::testing
