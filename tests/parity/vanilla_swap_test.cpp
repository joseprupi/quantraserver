// Vanilla Swap (incl. CMS) parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

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
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);
    
    // Fixed leg
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
    
    // Float leg — uses IndexRef
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
    
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::SwapFloatingLegBuilder flgb(b);
    flgb.add_notional(notional);
    flgb.add_schedule(floatSch);
    flgb.add_index(idx6m);
    flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flgb.add_spread(0.0);
    flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto floatLeg = flgb.Finish();
    
    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
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
    EXPECT_DOUBLE_EQ(-1.0, r->used_cms_mean_reversion());
}

TEST_F(QuantraComparisonTest, VanillaSwap_CMS_NPVMatches) {
    const double notional = 1000000.0;
    const double fixedRate = 0.035;
    const double cmsVol = 0.20;

    QuantLib::Date start = evaluationDate_ + 2;
    QuantLib::Date end = start + QuantLib::Period(5, QuantLib::Years);
    QuantLib::Schedule fixSch(
        start, end, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule cmsSch(
        start, end, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);

    auto ibor6m = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swapIndex = std::make_shared<QuantLib::SwapIndex>(
        "EUR_SWAP_10Y",
        QuantLib::Period(5, QuantLib::Years),
        2,
        ibor6m->currency(),
        QuantLib::TARGET(),
        QuantLib::Period(QuantLib::Annual),
        QuantLib::ModifiedFollowing,
        QuantLib::Thirty360(QuantLib::Thirty360::BondBasis),
        ibor6m,
        discountHandle_);

    QuantLib::Leg qlFixedLeg = QuantLib::FixedRateLeg(fixSch)
        .withNotionals(notional)
        .withCouponRates(fixedRate, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis))
        .withPaymentAdjustment(QuantLib::ModifiedFollowing);
    QuantLib::Leg qlCmsLeg = QuantLib::CmsLeg(cmsSch, swapIndex)
        .withNotionals(notional)
        .withPaymentDayCounter(QuantLib::Actual360())
        .withPaymentAdjustment(QuantLib::ModifiedFollowing)
        .withFixingDays(2)
        .withGearings(1.0)
        .withSpreads(0.0);

    auto qlCmsVol = std::make_shared<QuantLib::ConstantSwaptionVolatility>(
        evaluationDate_,
        QuantLib::TARGET(),
        QuantLib::ModifiedFollowing,
        cmsVol,
        QuantLib::Actual365Fixed(),
        QuantLib::ShiftedLognormal,
        0.0);
    auto meanReversion = QuantLib::Handle<QuantLib::Quote>(
        QuantLib::ext::make_shared<QuantLib::SimpleQuote>(0.03));
    auto cmsPricer = QuantLib::ext::make_shared<QuantLib::LinearTsrPricer>(
        QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(qlCmsVol),
        meanReversion,
        discountHandle_);
    QuantLib::setCouponPricer(qlCmsLeg, cmsPricer);

    std::vector<QuantLib::Leg> qlLegs{qlFixedLeg, qlCmsLeg};
    std::vector<bool> payer{true, false}; // Payer: pay fixed, receive CMS.
    auto qlSwap = std::make_shared<QuantLib::Swap>(qlLegs, payer);
    qlSwap->setPricingEngine(std::make_shared<QuantLib::DiscountingSwapEngine>(discountHandle_));
    const double qlNPV = qlSwap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto vol = buildSwaptionVolSurface(
        b, "cms_swaption_vol", cmsVol, quantra::enums::VolatilityType_Lognormal, 0.0, "", "2025-01-15", "EUR_SWAP_6M");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vol});
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, asof, 0, indices, swapIndices, curves, 0, 0, vols);

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

    auto cleff = b.CreateString("2025-01-17");
    auto clterm = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder cmsSb(b);
    cmsSb.add_effective_date(cleff);
    cmsSb.add_termination_date(clterm);
    cmsSb.add_calendar(quantra::enums::Calendar_TARGET);
    cmsSb.add_frequency(quantra::enums::Frequency_Semiannual);
    cmsSb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    cmsSb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    cmsSb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto cmsSchOff = cmsSb.Finish();

    auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto cmsSwapIndexId = b.CreateString("EUR_SWAP_6M");
    auto cmsVolId = b.CreateString("cms_swaption_vol");
    quantra::SwapCmsLegBuilder cmsLb(b);
    cmsLb.add_notional(notional);
    cmsLb.add_schedule(cmsSchOff);
    cmsLb.add_swap_index_id(cmsSwapIndexId);
    cmsLb.add_swap_tenor(cmsTenor);
    cmsLb.add_swaption_vol_id(cmsVolId);
    cmsLb.add_fixing_days(2);
    cmsLb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cmsLb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    cmsLb.add_gear(1.0);
    cmsLb.add_spread(0.0);
    auto cmsLeg = cmsLb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_cms_leg(cmsLeg);
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
    const auto out = flatbuffers::GetRoot<quantra::PriceVanillaSwapResponse>(respB->GetBufferPointer())->swaps()->Get(0);
    const double qNPV = out->npv();

    EXPECT_TRUE(std::isfinite(qNPV));
    EXPECT_NEAR(qlNPV, qNPV, 0.5);
}

TEST_F(QuantraComparisonTest, VanillaSwap_CMS_MissingVolThrows) {
    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves);

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
    auto sch = sb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(1000000.0);
    flb.add_schedule(sch);
    flb.add_rate(0.03);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto swapIndexId = b.CreateString("EUR_SWAP_6M");
    auto missingVolId = b.CreateString("missing_cms_vol");
    quantra::SwapCmsLegBuilder cmsLb(b);
    cmsLb.add_notional(1000000.0);
    cmsLb.add_schedule(sch);
    cmsLb.add_swap_index_id(swapIndexId);
    cmsLb.add_swap_tenor(cmsTenor);
    cmsLb.add_swaption_vol_id(missingVolId);
    cmsLb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cmsLb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto cmsLeg = cmsLb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_cms_leg(cmsLeg);
    auto swap = vsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceVanillaSwapBuilder pvsb(b);
    pvsb.add_vanilla_swap(swap);
    pvsb.add_discounting_curve(dc);
    pvsb.add_forwarding_curve(dc);
    auto pvs = pvsb.Finish();
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{pvs});
    quantra::PriceVanillaSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    VanillaSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceVanillaSwapRequest>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, VanillaSwap_CMS_UnknownSwapIndexThrows) {
    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto vol = buildSwaptionVolSurface(
        b, "cms_swaption_vol", 0.20, quantra::enums::VolatilityType_Lognormal, 0.0, "", "2025-01-15", "EUR_SWAP_6M");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vol});
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

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
    auto sch = sb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(1000000.0);
    flb.add_schedule(sch);
    flb.add_rate(0.03);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto swapIndexId = b.CreateString("UNKNOWN_SWAP_INDEX");
    auto volId = b.CreateString("cms_swaption_vol");
    quantra::SwapCmsLegBuilder cmsLb(b);
    cmsLb.add_notional(1000000.0);
    cmsLb.add_schedule(sch);
    cmsLb.add_swap_index_id(swapIndexId);
    cmsLb.add_swap_tenor(cmsTenor);
    cmsLb.add_swaption_vol_id(volId);
    cmsLb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cmsLb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto cmsLeg = cmsLb.Finish();

    quantra::VanillaSwapBuilder vsb(b);
    vsb.add_swap_type(quantra::enums::SwapType_Payer);
    vsb.add_fixed_leg(fixedLeg);
    vsb.add_cms_leg(cmsLeg);
    auto swap = vsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceVanillaSwapBuilder pvsb(b);
    pvsb.add_vanilla_swap(swap);
    pvsb.add_discounting_curve(dc);
    pvsb.add_forwarding_curve(dc);
    auto pvs = pvsb.Finish();
    auto swaps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceVanillaSwap>>{pvs});
    quantra::PriceVanillaSwapRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaps(swaps);
    b.Finish(rb.Finish());

    VanillaSwapPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceVanillaSwapRequest>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, CMSCoupons_PricerAttached) {
    flatbuffers::grpc::MessageBuilder pbuilder;
    auto ts = buildCurve(pbuilder, "discount");
    auto curves = pbuilder.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(pbuilder);
    auto swapIndices = buildSwapIndicesVector(pbuilder);
    auto vol = buildSwaptionVolSurface(
        pbuilder, "cms_swaption_vol", 0.20, quantra::enums::VolatilityType_Lognormal, 0.0, "", "2025-01-15", "EUR_SWAP_6M");
    auto vols = pbuilder.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vol});
    auto asof = pbuilder.CreateString("2025-01-15");

    auto pricing = buildPricing(pbuilder, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
    pbuilder.Finish(pricing);

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(
        flatbuffers::GetRoot<quantra::Pricing>(pbuilder.GetBufferPointer()));

    flatbuffers::grpc::MessageBuilder b;
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
    auto sch = sb.Finish();

    auto cmsTenor = buildPeriod(b, 5, quantra::enums::TimeUnit_Years);
    auto swapIndexId = b.CreateString("EUR_SWAP_6M");
    auto volId = b.CreateString("cms_swaption_vol");
    quantra::SwapCmsLegBuilder cmsLb(b);
    cmsLb.add_notional(1000000.0);
    cmsLb.add_schedule(sch);
    cmsLb.add_swap_index_id(swapIndexId);
    cmsLb.add_swap_tenor(cmsTenor);
    cmsLb.add_swaption_vol_id(volId);
    cmsLb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cmsLb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    cmsLb.add_fixing_days(2);
    auto cmsLegFb = cmsLb.Finish();

    b.Finish(cmsLegFb);
    auto cmsLegRoot = flatbuffers::GetRoot<quantra::SwapCmsLeg>(b.GetBufferPointer());

    auto dIt = reg.rates.curves.find("discount");
    ASSERT_TRUE(dIt != reg.rates.curves.end());
    Handle<YieldTermStructure> discountCurve(dIt->second->currentLink());

    CmsLegParser parser;
    auto cmsLeg = parser.parse(
        cmsLegRoot, reg.rates.indices, reg.rates.swapIndices, discountCurve, discountCurve);
    auto vIt = reg.volatility.swaptionVols.find("cms_swaption_vol");
    ASSERT_TRUE(vIt != reg.volatility.swaptionVols.end());
    auto pricerBuild = parser.makeCouponPricer(cmsLegRoot, vIt->second, discountCurve);
    QuantLib::setCouponPricer(cmsLeg, pricerBuild.pricer);

    int cmsCoupons = 0;
    for (const auto& cf : cmsLeg) {
        auto cpn = QuantLib::ext::dynamic_pointer_cast<QuantLib::CmsCoupon>(cf);
        if (cpn) {
            ++cmsCoupons;
            EXPECT_TRUE(static_cast<bool>(cpn->pricer()));
        }
    }
    EXPECT_GT(cmsCoupons, 0);
}

}} // namespace quantra::testing
