// Vanilla Swap (incl. CMS) parity tests on the `SwapLeg` wire.
//
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary. Each case builds the FlatBuffers
// request (two flat `SwapLeg`s carrying notional / schedule / day count /
// payment conventions, with FixedCoupons on `fixed_leg` and IborCoupons or
// CmsCoupons on `floating_leg`), prices it through the VanillaSwap handler
// and compares against an independently-built QuantLib::VanillaSwap / Swap.
//
// Contract: the shared leg mapper's validations are 400s naming the field
// (`VanillaSwap.fixed_leg...` for the leg-level fields,
// `...floating_leg.coupons...` for the kind-specific ones), the principal
// exchanges add exactly the PV of the notional flows, a payment lag on the
// IBOR leg reproduces QuantLib's lagged IborLeg, and the CMS leg builder
// attaches the requested pricer to every CMS coupon.
#include "date_convert.h"
#include "parity_fixture.h"
#include "pricing_context.h"
#include "swap_leg_pricer.h"

#include <ql/cashflows/cashflows.hpp>
#include <ql/cashflows/couponpricer.hpp>
#include <ql/cashflows/simplecashflow.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/time/calendars/unitedstates.hpp>

#include <functional>

namespace quantra { namespace testing {

class VanillaSwapParityTest : public QuantraComparisonTest {
protected:
    static constexpr double kNotional = 1000000.0;
    static constexpr double kFixedRate = 0.035;

    /// The 2025-01-17 .. 2030-01-17 TARGET schedule the swaps here share.
    flatbuffers::Offset<quantra::Schedule> fbSchedule(
        flatbuffers::grpc::MessageBuilder& b, quantra::enums::Frequency freq) {
        auto eff = b.CreateString("2025-01-17");
        auto term = b.CreateString("2030-01-17");
        quantra::ScheduleBuilder sb(b);
        sb.add_effective_date(eff);
        sb.add_termination_date(term);
        sb.add_calendar(quantra::enums::Calendar_TARGET);
        sb.add_frequency(freq);
        sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        sb.add_end_of_month(false);
        return sb.Finish();
    }

    /// The shared fields of the fixed leg: annual 30/360 coupons.
    SwapLegFields fixedFields(flatbuffers::grpc::MessageBuilder& b) {
        return SwapLegFields(
            fbSchedule(b, quantra::enums::Frequency_Annual), kNotional,
            quantra::enums::DayCounter_Thirty360);
    }

    /// The shared fields of the floating leg (IBOR or CMS): semiannual
    /// Act/360 coupons.
    SwapLegFields floatingFields(flatbuffers::grpc::MessageBuilder& b) {
        return SwapLegFields(
            fbSchedule(b, quantra::enums::Frequency_Semiannual), kNotional,
            quantra::enums::DayCounter_Actual360);
    }

    /// FixedCoupons at `rate`.
    flatbuffers::Offset<quantra::FixedCoupons> fbFixed(
        flatbuffers::grpc::MessageBuilder& b, double rate = kFixedRate) {
        quantra::FixedCouponsBuilder flb(b);
        flb.add_rate(rate);
        return flb.Finish();
    }

    /// Euribor 6M coupons at `spread` (zero by default).
    flatbuffers::Offset<quantra::IborCoupons> fbIbor(
        flatbuffers::grpc::MessageBuilder& b, double spread = 0.0) {
        auto idx6m = buildIndexRef(b, "EUR_6M");
        quantra::IborCouponsBuilder flgb(b);
        flgb.add_index(idx6m);
        flgb.add_spread(spread);
        return flgb.Finish();
    }

    /// CMS coupons on the 5Y EUR swap rate.
    flatbuffers::Offset<quantra::CmsCoupons> fbCms(
        flatbuffers::grpc::MessageBuilder& b,
        const char* swapIndexId = "EUR_SWAP_6M",
        const char* volId = "cms_swaption_vol") {
        auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
        auto swapIndexIdOff = b.CreateString(swapIndexId);
        auto volIdOff = b.CreateString(volId);
        quantra::CmsCouponsBuilder cmsLb(b);
        cmsLb.add_swap_index_id(swapIndexIdOff);
        cmsLb.add_swap_tenor(cmsTenor);
        cmsLb.add_swaption_vol_id(volIdOff);
        cmsLb.add_fixing_days(2);
        cmsLb.add_gear(1.0);
        cmsLb.add_spread(0.0);
        return cmsLb.Finish();
    }

    /// The default payer swap of fbFixed / fbIbor on one curve.
    flatbuffers::Offset<quantra::VanillaSwap> fbPayerSwap(
        flatbuffers::grpc::MessageBuilder& b, std::optional<bool> exchange = std::nullopt) {
        return buildVanillaSwap(
            b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbFixed(b), floatingFields(b),
            fbIbor(b), "discount", "discount", exchange);
    }

    /// The single EUR curve ("discount") + EUR_6M index; with `cms` also the
    /// EUR_SWAP_6M swap index and a 20% lognormal swaption vol
    /// ("cms_swaption_vol").
    flatbuffers::Offset<quantra::Pricing> fbPricing(
        flatbuffers::grpc::MessageBuilder& b, bool cms = false, bool withVol = true) {
        auto ts = buildCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        auto indices = buildIndicesVector(b);
        auto asof = b.CreateString("2025-01-15");
        if (!cms) return buildPricing(b, asof, asof, 0, indices, 0, curves);
        auto swapIndices = buildSwapIndicesVector(b);
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>>
            vols = 0;
        if (withVol) {
            auto vol = buildSwaptionVolSurface(
                b, "cms_swaption_vol", 0.20, quantra::enums::VolatilityType_Lognormal, 0.0, "",
                "2025-01-15", "EUR_SWAP_6M");
            vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vol});
        }
        return buildPricing(b, asof, asof, 0, indices, swapIndices, curves, 0, 0, vols);
    }

    /// Prices a one-swap request through the handler; the response buffer
    /// stays alive in `respB_` until the next call.
    const quantra::VanillaSwapResponse* price(
        flatbuffers::grpc::MessageBuilder& b,
        flatbuffers::Offset<quantra::Pricing> pricing,
        flatbuffers::Offset<quantra::VanillaSwap> swap,
        bool includeFlows = false) {
        auto entry = quantra::CreatePriceVanillaSwap(b, swap);
        auto swaps =
            b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{entry});
        b.Finish(quantra::CreatePriceVanillaSwapRequest(b, pricing, swaps, includeFlows));

        VanillaSwapPricingRequest req;
        respB_ = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(
            respB_, flatbuffers::GetRoot<quantra::PriceVanillaSwapRequest>(b.GetBufferPointer()));
        respB_->Finish(resp);
        return flatbuffers::GetRoot<quantra::PriceVanillaSwapResponse>(respB_->GetBufferPointer())
            ->swaps()
            ->Get(0);
    }

    std::string errorOf(const std::function<void()>& fn) {
        try {
            fn();
        } catch (const QuantraError& e) {
            return e.what();
        }
        return "";
    }

    /// The QuantLib twin of fbFixed / fbIbor: payer 5Y 3.5% vs Euribor 6M.
    std::shared_ptr<QuantLib::VanillaSwap> qlPayerSwap() {
        QuantLib::Date start = evaluationDate_ + 2;
        QuantLib::Date end = start + QuantLib::Period(5, QuantLib::Years);
        QuantLib::Schedule fixSch(
            start, end, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
            QuantLib::DateGeneration::Forward, false);
        QuantLib::Schedule fltSch(
            start, end, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
            QuantLib::DateGeneration::Forward, false);
        auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
        auto swap = std::make_shared<QuantLib::VanillaSwap>(
            QuantLib::VanillaSwap::Payer, kNotional, fixSch, kFixedRate,
            QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0,
            QuantLib::Actual360());
        swap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
        return swap;
    }

    std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB_;
};

TEST_F(VanillaSwapParityTest, VanillaSwap_NPVMatches) {
    std::cout << "\n=== Vanilla Swap ===" << std::endl;
    auto qlSwap = qlPayerSwap();
    const double qlNPV = qlSwap->NPV();
    const double qlFairRate = qlSwap->fairRate();

    flatbuffers::grpc::MessageBuilder b;
    auto pricing = fbPricing(b);
    // Payer: the fixed leg is paid, Euribor received; both legs in EUR on the
    // one curve, which also projects the floating leg.
    auto swap = fbPayerSwap(b);
    const auto* r = price(b, pricing, swap);
    ASSERT_TRUE(r->fair_rate().has_value());
    const double qNPV = r->npv();
    const double qFairRate = r->fair_rate().value();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV
              << " | Diff: " << std::abs(qlNPV - qNPV) << std::endl;
    std::cout << "QuantLib Fair: " << qlFairRate * 100 << "% | Quantra: " << qFairRate * 100 << "%"
              << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFairRate, qFairRate, 1e-6);
    // legs[0] = fixed_leg, legs[1] = floating_leg, each signed from the
    // reported party's view exactly as QuantLib signs its leg NPVs.
    ASSERT_EQ(r->legs()->size(), 2u);
    EXPECT_NEAR(qlSwap->fixedLegNPV(), r->legs()->Get(0)->npv(), 0.01);
    EXPECT_NEAR(qlSwap->floatingLegNPV(), r->legs()->Get(1)->npv(), 0.01);
    EXPECT_NEAR(r->npv(), r->legs()->Get(0)->npv() + r->legs()->Get(1)->npv(), 1e-9);
    EXPECT_FALSE(r->legs()->Get(0)->flows());
    // No CMS leg: the CMS diagnostic is omitted entirely rather than emitting a
    // -1.0 sentinel, so the optional scalar has no value.
    EXPECT_FALSE(r->used_cms_mean_reversion().has_value());
}

// fair_spread is QuantLib::VanillaSwap::fairSpread (the spread on the
// floating leg that zeroes the swap, the fixed leg held fixed), and the swap
// repriced through the API with that spread has a zero NPV.
TEST_F(VanillaSwapParityTest, VanillaSwap_FairSpreadMatchesQuantLibAndReprices) {
    auto qlSwap = qlPayerSwap();
    const double qlFairSpread = qlSwap->fairSpread();

    flatbuffers::grpc::MessageBuilder b;
    auto pricing = fbPricing(b);
    const auto* r = price(b, pricing, fbPayerSwap(b));
    ASSERT_TRUE(r->fair_spread().has_value());
    EXPECT_NEAR(qlFairSpread, r->fair_spread().value(), 1e-12);
    const double fairSpread = r->fair_spread().value();

    flatbuffers::grpc::MessageBuilder b2;
    auto pricing2 = fbPricing(b2);
    auto atFair = buildVanillaSwap(
        b2, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b2), fbFixed(b2), floatingFields(b2),
        fbIbor(b2, fairSpread), "discount", "discount");
    const auto* r2 = price(b2, pricing2, atFair);
    EXPECT_LT(std::abs(r2->npv()), 1e-9 * kNotional);
    ASSERT_TRUE(r2->fair_spread().has_value());
    EXPECT_NEAR(fairSpread, r2->fair_spread().value(), 1e-12);
    // Both fair quantities are reported on every vanilla swap.
    ASSERT_TRUE(r2->fair_rate().has_value());
}

TEST_F(VanillaSwapParityTest, VanillaSwap_LegRulesAre400s) {
    // Every leg-model rule is a 400 naming the offending field, raised by the
    // mapper before any market data is touched.
    auto check = [&](const std::function<flatbuffers::Offset<quantra::VanillaSwap>(
                         flatbuffers::grpc::MessageBuilder&)>& makeSwap,
                     const char* expected) {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = fbPricing(b);
        auto swap = makeSwap(b);
        const std::string msg = errorOf([&] { price(b, pricing, swap); });
        EXPECT_NE(msg.find(expected), std::string::npos)
            << "expected '" << expected << "' in '" << msg << "'";
    };
    // IborCoupons on the fixed leg: the role fixes the coupon kind.
    check(
        [&](flatbuffers::grpc::MessageBuilder& b) {
            auto fixedLeg = buildSwapLeg(
                b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbIbor(b), "discount",
                "discount");
            auto floatingLeg = buildSwapLeg(
                b, quantra::enums::PayReceive_Receive, "EUR", floatingFields(b), fbIbor(b),
                "discount", "discount");
            return quantra::CreateVanillaSwap(b, fixedLeg, floatingLeg);
        },
        "VanillaSwap.fixed_leg.coupons must be FixedCoupons");
    // Both legs paid.
    check(
        [&](flatbuffers::grpc::MessageBuilder& b) {
            auto fixedLeg = buildSwapLeg(
                b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbFixed(b), "discount");
            auto floatingLeg = buildSwapLeg(
                b, quantra::enums::PayReceive_Pay, "EUR", floatingFields(b), fbIbor(b), "discount",
                "discount");
            return quantra::CreateVanillaSwap(b, fixedLeg, floatingLeg);
        },
        "VanillaSwap.fixed_leg.pay_receive and VanillaSwap.floating_leg.pay_receive are both "
        "Pay");
    // Two currencies: that is a cross-currency swap, priced elsewhere.
    check(
        [&](flatbuffers::grpc::MessageBuilder& b) {
            auto fixedLeg = buildSwapLeg(
                b, quantra::enums::PayReceive_Pay, "USD", fixedFields(b), fbFixed(b), "discount");
            auto floatingLeg = buildSwapLeg(
                b, quantra::enums::PayReceive_Receive, "EUR", floatingFields(b), fbIbor(b),
                "discount", "discount");
            return quantra::CreateVanillaSwap(b, fixedLeg, floatingLeg);
        },
        "price-cross-currency-swap");
    // A forwarding curve on the fixed leg: a fixed leg projects nothing.
    check(
        [&](flatbuffers::grpc::MessageBuilder& b) {
            auto fixedLeg = buildSwapLeg(
                b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbFixed(b), "discount",
                "discount");
            auto floatingLeg = buildSwapLeg(
                b, quantra::enums::PayReceive_Receive, "EUR", floatingFields(b), fbIbor(b),
                "discount", "discount");
            return quantra::CreateVanillaSwap(b, fixedLeg, floatingLeg);
        },
        "VanillaSwap.fixed_leg.forwarding_curve must be absent with FixedCoupons");
    // The shared fields live on the leg: a missing notional / day_counter /
    // payment_convention is named with the leg's path.
    auto withoutField = [&](const std::function<void(SwapLegFields&)>& drop, const char* field) {
        check(
            [&](flatbuffers::grpc::MessageBuilder& b) {
                SwapLegFields fields = fixedFields(b);
                drop(fields);
                auto fixedLeg = buildSwapLeg(
                    b, quantra::enums::PayReceive_Pay, "EUR", fields, fbFixed(b), "discount");
                auto floatingLeg = buildSwapLeg(
                    b, quantra::enums::PayReceive_Receive, "EUR", floatingFields(b), fbIbor(b),
                    "discount", "discount");
                return quantra::CreateVanillaSwap(b, fixedLeg, floatingLeg);
            },
            field);
    };
    withoutField([](SwapLegFields& f) { f.notional.reset(); }, "VanillaSwap.fixed_leg.notional");
    // (`schedule` is (required) in the schema: the FlatBuffers builder asserts
    // on a leg without one; the JSON parser rejects it in the contract suite.)
    withoutField(
        [](SwapLegFields& f) { f.dayCounter.reset(); }, "VanillaSwap.fixed_leg.day_counter");
    withoutField(
        [](SwapLegFields& f) { f.paymentConvention.reset(); },
        "VanillaSwap.fixed_leg.payment_convention");
}

TEST_F(VanillaSwapParityTest, VanillaSwap_PaymentLagOnCmsCouponsIs400) {
    // A CMS leg pays on its schedule dates: payment_lag / payment_calendar on
    // the leg are rejected (the message names the field and CmsCoupons).
    flatbuffers::grpc::MessageBuilder b;
    auto pricing = fbPricing(b, true);
    SwapLegFields cmsFields = floatingFields(b);
    cmsFields.paymentLag = 2;
    auto fixedLeg = buildSwapLeg(
        b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbFixed(b), "discount");
    auto floatingLeg = buildSwapLeg(
        b, quantra::enums::PayReceive_Receive, "EUR", cmsFields, fbCms(b), "discount", "discount");
    auto swap = quantra::CreateVanillaSwap(b, fixedLeg, floatingLeg);
    const std::string msg = errorOf([&] { price(b, pricing, swap); });
    EXPECT_NE(msg.find("VanillaSwap.floating_leg.payment_lag"), std::string::npos) << msg;
    EXPECT_NE(msg.find("not supported with CmsCoupons"), std::string::npos) << msg;
}

TEST_F(VanillaSwapParityTest, VanillaSwap_IborLegPaymentLag_MatchesQuantLib) {
    // payment_lag / payment_calendar on the floating leg reproduce QuantLib's
    // IborLeg().withPaymentLag(2).withPaymentCalendar(US GovernmentBond):
    // every floating coupon pays two US business days after its accrual end
    // while the fixed leg keeps paying on its TARGET-adjusted accrual ends.
    const int paymentLag = 2;
    const QuantLib::Calendar usGov(QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond));
    QuantLib::Date start = evaluationDate_ + 2;
    QuantLib::Date end = start + QuantLib::Period(5, QuantLib::Years);
    QuantLib::Schedule fixSch(
        start, end, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward,
        false);
    QuantLib::Schedule fltSch(
        start, end, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward,
        false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    QuantLib::Leg qlFixedLeg =
        QuantLib::FixedRateLeg(fixSch)
            .withNotionals(kNotional)
            .withCouponRates(kFixedRate, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis))
            .withPaymentAdjustment(QuantLib::ModifiedFollowing);
    QuantLib::Leg qlFloatLeg = QuantLib::IborLeg(fltSch, idx)
                                   .withNotionals(kNotional)
                                   .withPaymentDayCounter(QuantLib::Actual360())
                                   .withPaymentAdjustment(QuantLib::ModifiedFollowing)
                                   .withPaymentCalendar(usGov)
                                   .withPaymentLag(paymentLag)
                                   .withSpreads(0.0);
    // The engine prices IBOR coupons with a zero-vol Black pricer (no
    // convexity adjustment in advance): the same pricer here.
    auto optionletVol = std::make_shared<QuantLib::ConstantOptionletVolatility>(
        0, QuantLib::TARGET(), QuantLib::ModifiedFollowing, 0.0, QuantLib::Actual365Fixed());
    auto pricer = std::make_shared<QuantLib::BlackIborCouponPricer>();
    pricer->setCapletVolatility(
        QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(optionletVol));
    QuantLib::setCouponPricer(qlFloatLeg, pricer);
    std::vector<QuantLib::Leg> qlLegs{qlFixedLeg, qlFloatLeg};
    std::vector<bool> payer{true, false}; // Payer: pay fixed, receive floating.
    auto qlSwap = std::make_shared<QuantLib::Swap>(qlLegs, payer);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));

    flatbuffers::grpc::MessageBuilder b;
    auto pricing = fbPricing(b);
    SwapLegFields lagged = floatingFields(b).withPaymentTerms(
        quantra::enums::Calendar_UnitedStatesGovernmentBond, paymentLag);
    auto swap = buildVanillaSwap(
        b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbFixed(b), lagged, fbIbor(b),
        "discount", "discount");
    const auto* r = price(b, pricing, swap, true);

    EXPECT_NEAR(qlSwap->NPV(), r->npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(0), r->legs()->Get(0)->npv(), 0.01);
    EXPECT_NEAR(qlSwap->legNPV(1), r->legs()->Get(1)->npv(), 0.01);
    // The lag is not cosmetic: the plain swap (paying on the accrual ends)
    // values differently.
    flatbuffers::grpc::MessageBuilder b2;
    auto plainPricing = fbPricing(b2);
    const double plainNpv = price(b2, plainPricing, fbPayerSwap(b2))->npv();
    EXPECT_GT(std::abs(plainNpv - r->npv()), 1.0);
    // Every floating flow pays on QuantLib's lagged date.
    const auto* flows = r->legs()->Get(1)->flows();
    ASSERT_TRUE(flows);
    ASSERT_EQ(flows->size(), qlFloatLeg.size());
    for (unsigned i = 0; i < flows->size(); ++i) {
        const auto* f = flows->Get(i);
        const QuantLib::Date accrualEnd = DateToQL(f->accrual_end_date()->str());
        EXPECT_EQ(
            f->payment_date()->str(),
            DateToIso(usGov.advance(accrualEnd, paymentLag, QuantLib::Days)))
            << "flow " << i;
        EXPECT_EQ(f->payment_date()->str(), DateToIso(qlFloatLeg[i]->date())) << "flow " << i;
    }
}

TEST_F(VanillaSwapParityTest, VanillaSwap_NotionalExchangesAddTheirPv) {
    // With initial_exchange / final_exchange on both legs each leg gains, in
    // its own frame, -N on the schedule start and +N with its last coupon.
    // Reference: the plain swap's leg NPVs plus those two SimpleCashFlows
    // discounted on the same curve (flows on the as-of date excluded, as
    // the DiscountingSwapEngine does), signed with the leg's side.
    auto qlSwap = qlPayerSwap();
    auto exchangePv = [&](const QuantLib::Leg& coupons) {
        QuantLib::Leg flows{
            std::make_shared<QuantLib::SimpleCashFlow>(-kNotional, evaluationDate_ + 2),
            std::make_shared<QuantLib::SimpleCashFlow>(kNotional, coupons.back()->date())};
        return QuantLib::CashFlows::npv(
            flows, *bootstrappedCurve_, false, evaluationDate_, evaluationDate_);
    };
    const double fixedExchanges = exchangePv(qlSwap->fixedLeg());
    const double floatExchanges = exchangePv(qlSwap->floatingLeg());

    flatbuffers::grpc::MessageBuilder b1, b2;
    auto plain = fbPricing(b1);
    const auto* without = price(b1, plain, fbPayerSwap(b1));
    const double plainNpv = without->npv();
    const double plainFixed = without->legs()->Get(0)->npv();
    const double plainFloat = without->legs()->Get(1)->npv();

    auto withExchanges = fbPricing(b2);
    const auto* with = price(b2, withExchanges, fbPayerSwap(b2, true), true);

    // Pay fixed: -(exchanges); receive floating: +(exchanges).
    EXPECT_NEAR(with->legs()->Get(0)->npv(), plainFixed - fixedExchanges, 1e-6);
    EXPECT_NEAR(with->legs()->Get(1)->npv(), plainFloat + floatExchanges, 1e-6);
    EXPECT_NEAR(with->npv(), plainNpv - fixedExchanges + floatExchanges, 1e-6);
    // A bond bought below par on one side, a par floater on the other: the
    // exchanges move each leg by N * (DF(end) - DF(start)), not by zero.
    EXPECT_GT(std::abs(fixedExchanges), 1e-3 * kNotional);

    // The flows list the coupons first, then the two exchanges with their kind.
    for (unsigned i = 0; i < 2; ++i) {
        const auto* flows = with->legs()->Get(i)->flows();
        ASSERT_TRUE(flows);
        const unsigned n = flows->size();
        ASSERT_EQ(n, (i == 0 ? 5u : 10u) + 2u);
        unsigned exchanges = 0;
        for (unsigned k = 0; k < n; ++k) {
            ASSERT_TRUE(flows->Get(k)->kind().has_value());
            if (flows->Get(k)->kind().value() == quantra::enums::SwapFlowKind_NotionalExchange) {
                ++exchanges;
            } else {
                EXPECT_EQ(flows->Get(k)->kind().value(), quantra::enums::SwapFlowKind_Coupon);
            }
        }
        EXPECT_EQ(exchanges, 2u);
        EXPECT_DOUBLE_EQ(flows->Get(n - 2)->amount(), -kNotional);
        EXPECT_EQ(flows->Get(n - 2)->payment_date()->str(), "2025-01-17");
        EXPECT_DOUBLE_EQ(flows->Get(n - 1)->amount(), kNotional);
    }
}

TEST_F(VanillaSwapParityTest, VanillaSwap_CMS_NPVMatches) {
    const double cmsVol = 0.20;

    QuantLib::Date start = evaluationDate_ + 2;
    QuantLib::Date end = start + QuantLib::Period(5, QuantLib::Years);
    QuantLib::Schedule fixSch(
        start, end, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward,
        false);
    QuantLib::Schedule cmsSch(
        start, end, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward,
        false);

    auto ibor6m = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swapIndex = std::make_shared<QuantLib::SwapIndex>(
        "EUR_SWAP_10Y", QuantLib::Period(5, QuantLib::Years), 2, ibor6m->currency(),
        QuantLib::TARGET(), QuantLib::Period(QuantLib::Annual), QuantLib::ModifiedFollowing,
        QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), ibor6m, discountHandle_);

    QuantLib::Leg qlFixedLeg =
        QuantLib::FixedRateLeg(fixSch)
            .withNotionals(kNotional)
            .withCouponRates(kFixedRate, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis))
            .withPaymentAdjustment(QuantLib::ModifiedFollowing);
    QuantLib::Leg qlCmsLeg = QuantLib::CmsLeg(cmsSch, swapIndex)
                                 .withNotionals(kNotional)
                                 .withPaymentDayCounter(QuantLib::Actual360())
                                 .withPaymentAdjustment(QuantLib::ModifiedFollowing)
                                 .withFixingDays(2)
                                 .withGearings(1.0)
                                 .withSpreads(0.0);

    auto qlCmsVol = std::make_shared<QuantLib::ConstantSwaptionVolatility>(
        evaluationDate_, QuantLib::TARGET(), QuantLib::ModifiedFollowing, cmsVol,
        QuantLib::Actual365Fixed(), QuantLib::ShiftedLognormal, 0.0);
    auto meanReversion =
        QuantLib::Handle<QuantLib::Quote>(QuantLib::ext::make_shared<QuantLib::SimpleQuote>(0.03));
    auto cmsPricer = QuantLib::ext::make_shared<QuantLib::LinearTsrPricer>(
        QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlCmsVol), meanReversion,
        discountHandle_);
    QuantLib::setCouponPricer(qlCmsLeg, cmsPricer);

    std::vector<QuantLib::Leg> qlLegs{qlFixedLeg, qlCmsLeg};
    std::vector<bool> payer{true, false}; // Payer: pay fixed, receive CMS.
    auto qlSwap = std::make_shared<QuantLib::Swap>(qlLegs, payer);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto pricing = fbPricing(b, true);
    // A CMS swap is a vanilla swap whose floating leg carries CmsCoupons.
    auto swap = buildVanillaSwap(
        b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbFixed(b), floatingFields(b),
        fbCms(b), "discount", "discount");
    const auto* out = price(b, pricing, swap);
    const double qNPV = out->npv();

    EXPECT_TRUE(std::isfinite(qNPV));
    EXPECT_NEAR(qlNPV, qNPV, 0.5);
    EXPECT_NEAR(qlSwap->legNPV(0), out->legs()->Get(0)->npv(), 0.5);
    EXPECT_NEAR(qlSwap->legNPV(1), out->legs()->Get(1)->npv(), 0.5);
    // The fixed rate that zeroes the swap against the CMS leg is reported.
    ASSERT_TRUE(out->fair_rate().has_value());
    EXPECT_TRUE(std::isfinite(out->fair_rate().value()));
    EXPECT_NEAR(out->fair_rate().value(), kFixedRate - qlNPV / (qlSwap->legBPS(0) / 1e-4), 1e-6);
    // The spread on the CMS leg that zeroes the swap (the fixed leg held
    // fixed) is reported too, from the CMS coupons' annuity.
    ASSERT_TRUE(out->fair_spread().has_value());
    EXPECT_NEAR(out->fair_spread().value(), 0.0 - qlNPV / (qlSwap->legBPS(1) / 1e-4), 1e-6);
    // The effective CMS pricer settings are echoed back.
    ASSERT_TRUE(out->used_cms_mean_reversion().has_value());
    EXPECT_DOUBLE_EQ(out->used_cms_mean_reversion().value(), 0.03);
}

TEST_F(VanillaSwapParityTest, VanillaSwap_CMS_MissingVolThrows) {
    flatbuffers::grpc::MessageBuilder b;
    auto pricing = fbPricing(b, true, false);
    auto swap = buildVanillaSwap(
        b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbFixed(b, 0.03),
        floatingFields(b), fbCms(b, "EUR_SWAP_6M", "missing_cms_vol"), "discount", "discount");
    EXPECT_THROW(price(b, pricing, swap), QuantraError);
}

TEST_F(VanillaSwapParityTest, VanillaSwap_CMS_UnknownSwapIndexThrows) {
    flatbuffers::grpc::MessageBuilder b;
    auto pricing = fbPricing(b, true);
    auto swap = buildVanillaSwap(
        b, quantra::enums::PayReceive_Pay, "EUR", fixedFields(b), fbFixed(b, 0.03),
        floatingFields(b), fbCms(b, "UNKNOWN_SWAP_INDEX"), "discount", "discount");
    EXPECT_THROW(price(b, pricing, swap), QuantraError);
}

TEST_F(VanillaSwapParityTest, CMSCoupons_PricerAttached) {
    // The shared leg builder (swap_leg_pricer.h) attaches the requested CMS
    // coupon pricer to every CmsCoupon of a CmsCoupons leg and reports the
    // settings it used.
    flatbuffers::grpc::MessageBuilder pbuilder;
    pbuilder.Finish(fbPricing(pbuilder, true));
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg =
        regBuilder.build(flatbuffers::GetRoot<quantra::Pricing>(pbuilder.GetBufferPointer()));
    PricingContext ctx;
    ctx.asOf = evaluationDate_;

    SwapLegData leg;
    leg.kind = SwapLegData::Kind::Cms;
    leg.side = SwapLegData::Side::Receive;
    leg.currency = "EUR";
    leg.notional = kNotional;
    leg.cms.schedule = QuantLib::Schedule(
        QuantLib::Date(17, QuantLib::January, 2025), QuantLib::Date(17, QuantLib::January, 2030),
        QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(), QuantLib::ModifiedFollowing,
        QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    leg.cms.notional = kNotional;
    leg.cms.swapIndexId = "EUR_SWAP_6M";
    leg.cms.swapTenor = QuantLib::Period(5, QuantLib::Years);
    leg.cms.swaptionVolId = "cms_swaption_vol";
    leg.cms.fixingDays = 2;
    leg.cms.dayCounter = QuantLib::Actual360();
    leg.cms.paymentConvention = QuantLib::ModifiedFollowing;
    leg.discountingCurveId = "discount";
    leg.forwardingCurveId = "discount";

    const BuiltSwapLeg built = BuildSwapLeg(leg, "VanillaSwap.floating_leg", reg, ctx);

    int cmsCoupons = 0;
    for (const auto& cf : built.coupons) {
        auto cpn = QuantLib::ext::dynamic_pointer_cast<QuantLib::CmsCoupon>(cf);
        if (cpn) {
            ++cmsCoupons;
            EXPECT_TRUE(static_cast<bool>(cpn->pricer()));
        }
    }
    EXPECT_EQ(cmsCoupons, 10);
    EXPECT_TRUE(std::isfinite(built.npvOwn));
    ASSERT_TRUE(built.cmsUsed.has_value());
    EXPECT_EQ(built.cmsUsed->pricerType, quantra::enums::CmsPricerType_LinearTsr);
    EXPECT_DOUBLE_EQ(built.cmsUsed->meanReversion, 0.03);
}

}} // namespace quantra::testing
