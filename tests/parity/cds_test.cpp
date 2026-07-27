// CDS parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

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
    auto indices = buildIndicesVector(b);  // needed by SwapHelpers in the curve
    auto asof = b.CreateString("2025-01-15");
    auto credit_id = b.CreateString("credit");
    auto discount_id = b.CreateString("discount");
    
    quantra::CdsHelperConventionsBuilder hcb(b);
    hcb.add_settlement_days(0);
    hcb.add_frequency(quantra::enums::Frequency_Quarterly);
    hcb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    hcb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    hcb.add_last_period_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    hcb.add_settles_accrual(true);
    hcb.add_pays_at_default_time(true);
    hcb.add_rebates_accrual(true);
    hcb.add_helper_model(quantra::enums::CdsHelperModel_MidPoint);
    auto helper_conv = hcb.Finish();
    auto empty_quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CdsQuote>>{});

    quantra::CreditCurveSpecBuilder ccb(b);
    ccb.add_id(credit_id);
    ccb.add_reference_date(asof);
    ccb.add_calendar(quantra::enums::Calendar_TARGET);
    ccb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    ccb.add_recovery_rate(recovery);
    ccb.add_curve_interpolator(quantra::enums::Interpolator_LogLinear);
    ccb.add_helper_conventions(helper_conv);
    ccb.add_quotes(empty_quotes);
    ccb.add_flat_hazard_rate(hazard);
    auto cc = ccb.Finish();
    auto credit_curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CreditCurveSpec>>{cc});

    quantra::CdsModelSpecBuilder cmsb(b);
    cmsb.add_engine_type(quantra::enums::CdsEngineType_MidPoint);
    auto cds_payload = cmsb.Finish();
    auto model_id = b.CreateString("cds_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(model_id);
    msb.add_payload_type(quantra::ModelPayload_CdsModelSpec);
    msb.add_payload(cds_payload.Union());
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{msb.Finish()});

    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, 0, credit_curves, 0, models);
    
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
    sb.add_end_of_month(false);
    auto schedule = sb.Finish();
    
    quantra::CDSBuilder cdsb(b);
    cdsb.add_side(quantra::enums::ProtectionSide_Buyer);
    cdsb.add_notional(notional);
    cdsb.add_running_coupon(spread);
    cdsb.add_schedule(schedule);
    cdsb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    cdsb.add_settles_accrual(true);
    cdsb.add_pays_at_default_time(true);
    cdsb.add_rebates_accrual(true);
    cdsb.add_last_period_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_cash_settlement_days(3);
    auto cds = cdsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceCDSBuilder pcdsb(b);
    pcdsb.add_cds(cds);
    pcdsb.add_discounting_curve(dc);
    pcdsb.add_credit_curve_id(credit_id);
    pcdsb.add_model(model_id);
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
    double qFair = r->fair_spread().value();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    std::cout << "QuantLib Fair: " << qlFair*10000 << "bps | Quantra: " << qFair*10000 << "bps" << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFair, qFair, 1e-6);
}

namespace {

// Build a CDS request mirroring the base case but parameterised over the fields
// that vary across cases, and price it through the handler. The QuantLib
// reference (CreditDefaultSwap + FlatHazardRate + MidPointCdsEngine) is built
// here too. Members of QuantraComparisonTest are reachable because this is
// invoked from inside TEST_F bodies (passed `*this`).
struct CdsCase {
    quantra::enums::ProtectionSide fbSide;
    QuantLib::Protection::Side qlSide;
    double notional;
    double runningCoupon;
    double recovery;
    double hazard;
    quantra::enums::Frequency fbFreq;
    QuantLib::Frequency qlFreq;
};

} // namespace

// Seller side (sign flip vs the Buyer base case), wider running coupon and a
// higher hazard rate.
TEST_F(QuantraComparisonTest, CDS_Seller_NPVMatches) {
    const CdsCase c{quantra::enums::ProtectionSide_Seller, QuantLib::Protection::Seller,
                    10000000.0, 0.015, 0.40, 0.03,
                    quantra::enums::Frequency_Quarterly, QuantLib::Quarterly};

    QuantLib::Schedule sch(evaluationDate_, evaluationDate_ + QuantLib::Period(5, QuantLib::Years),
        QuantLib::Period(c.qlFreq), QuantLib::TARGET(),
        QuantLib::Following, QuantLib::Unadjusted, QuantLib::DateGeneration::TwentiethIMM, false);
    auto qlCDS = std::make_shared<QuantLib::CreditDefaultSwap>(c.qlSide,
        c.notional, c.runningCoupon, sch, QuantLib::Following, QuantLib::Actual360());
    auto defCurve = std::make_shared<QuantLib::FlatHazardRate>(evaluationDate_, c.hazard, QuantLib::Actual365Fixed());
    qlCDS->setPricingEngine(std::make_shared<QuantLib::MidPointCdsEngine>(
        QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure>(defCurve), c.recovery, discountHandle_));
    const double qlNPV = qlCDS->NPV();
    const double qlFair = qlCDS->fairSpread();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto credit_id = b.CreateString("credit");

    quantra::CdsHelperConventionsBuilder hcb(b);
    hcb.add_settlement_days(0);
    hcb.add_frequency(quantra::enums::Frequency_Quarterly);
    hcb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    hcb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    hcb.add_last_period_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    hcb.add_settles_accrual(true);
    hcb.add_pays_at_default_time(true);
    hcb.add_rebates_accrual(true);
    hcb.add_helper_model(quantra::enums::CdsHelperModel_MidPoint);
    auto helper_conv = hcb.Finish();
    auto empty_quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CdsQuote>>{});

    quantra::CreditCurveSpecBuilder ccb(b);
    ccb.add_id(credit_id);
    ccb.add_reference_date(asof);
    ccb.add_calendar(quantra::enums::Calendar_TARGET);
    ccb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    ccb.add_recovery_rate(c.recovery);
    ccb.add_curve_interpolator(quantra::enums::Interpolator_LogLinear);
    ccb.add_helper_conventions(helper_conv);
    ccb.add_quotes(empty_quotes);
    ccb.add_flat_hazard_rate(c.hazard);
    auto cc = ccb.Finish();
    auto credit_curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CreditCurveSpec>>{cc});

    quantra::CdsModelSpecBuilder cmsb(b);
    cmsb.add_engine_type(quantra::enums::CdsEngineType_MidPoint);
    auto cds_payload = cmsb.Finish();
    auto model_id = b.CreateString("cds_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(model_id);
    msb.add_payload_type(quantra::ModelPayload_CdsModelSpec);
    msb.add_payload(cds_payload.Union());
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{msb.Finish()});

    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, 0, credit_curves, 0, models);

    auto eff = b.CreateString("2025-01-15");
    auto term = b.CreateString("2030-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(c.fbFreq);
    sb.add_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    sb.add_end_of_month(false);
    auto schedule = sb.Finish();

    quantra::CDSBuilder cdsb(b);
    cdsb.add_side(c.fbSide);
    cdsb.add_notional(c.notional);
    cdsb.add_running_coupon(c.runningCoupon);
    cdsb.add_schedule(schedule);
    cdsb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    cdsb.add_settles_accrual(true);
    cdsb.add_pays_at_default_time(true);
    cdsb.add_rebates_accrual(true);
    cdsb.add_last_period_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_cash_settlement_days(3);
    auto cds = cdsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceCDSBuilder pcdsb(b);
    pcdsb.add_cds(cds);
    pcdsb.add_discounting_curve(dc);
    pcdsb.add_credit_curve_id(credit_id);
    pcdsb.add_model(model_id);
    auto cdss = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceCDS>>{pcdsb.Finish()});

    quantra::PriceCDSRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_cds_list(cdss);
    b.Finish(rb.Finish());

    CDSPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceCDSRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceCDSResponse>(respB->GetBufferPointer())->cds_list()->Get(0);
    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlFair, r->fair_spread().value(), 1e-6);
}

// Buyer, semiannual premium schedule and a low recovery (0.25) — different
// premium frequency and recovery from the base/seller cases.
TEST_F(QuantraComparisonTest, CDS_Semiannual_LowRecovery) {
    const CdsCase c{quantra::enums::ProtectionSide_Buyer, QuantLib::Protection::Buyer,
                    5000000.0, 0.012, 0.25, 0.025,
                    quantra::enums::Frequency_Semiannual, QuantLib::Semiannual};

    QuantLib::Schedule sch(evaluationDate_, evaluationDate_ + QuantLib::Period(5, QuantLib::Years),
        QuantLib::Period(c.qlFreq), QuantLib::TARGET(),
        QuantLib::Following, QuantLib::Unadjusted, QuantLib::DateGeneration::TwentiethIMM, false);
    auto qlCDS = std::make_shared<QuantLib::CreditDefaultSwap>(c.qlSide,
        c.notional, c.runningCoupon, sch, QuantLib::Following, QuantLib::Actual360());
    auto defCurve = std::make_shared<QuantLib::FlatHazardRate>(evaluationDate_, c.hazard, QuantLib::Actual365Fixed());
    qlCDS->setPricingEngine(std::make_shared<QuantLib::MidPointCdsEngine>(
        QuantLib::Handle<QuantLib::DefaultProbabilityTermStructure>(defCurve), c.recovery, discountHandle_));
    const double qlNPV = qlCDS->NPV();
    const double qlFair = qlCDS->fairSpread();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto credit_id = b.CreateString("credit");

    quantra::CdsHelperConventionsBuilder hcb(b);
    hcb.add_settlement_days(0);
    hcb.add_frequency(quantra::enums::Frequency_Quarterly);
    hcb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    hcb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    hcb.add_last_period_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    hcb.add_settles_accrual(true);
    hcb.add_pays_at_default_time(true);
    hcb.add_rebates_accrual(true);
    hcb.add_helper_model(quantra::enums::CdsHelperModel_MidPoint);
    auto helper_conv = hcb.Finish();
    auto empty_quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CdsQuote>>{});

    quantra::CreditCurveSpecBuilder ccb(b);
    ccb.add_id(credit_id);
    ccb.add_reference_date(asof);
    ccb.add_calendar(quantra::enums::Calendar_TARGET);
    ccb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    ccb.add_recovery_rate(c.recovery);
    ccb.add_curve_interpolator(quantra::enums::Interpolator_LogLinear);
    ccb.add_helper_conventions(helper_conv);
    ccb.add_quotes(empty_quotes);
    ccb.add_flat_hazard_rate(c.hazard);
    auto cc = ccb.Finish();
    auto credit_curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::CreditCurveSpec>>{cc});

    quantra::CdsModelSpecBuilder cmsb(b);
    cmsb.add_engine_type(quantra::enums::CdsEngineType_MidPoint);
    auto cds_payload = cmsb.Finish();
    auto model_id = b.CreateString("cds_model");
    quantra::ModelSpecBuilder msb(b);
    msb.add_id(model_id);
    msb.add_payload_type(quantra::ModelPayload_CdsModelSpec);
    msb.add_payload(cds_payload.Union());
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{msb.Finish()});

    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves, 0, credit_curves, 0, models);

    auto eff = b.CreateString("2025-01-15");
    auto term = b.CreateString("2030-01-15");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(c.fbFreq);
    sb.add_convention(quantra::enums::BusinessDayConvention_Following);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_TwentiethIMM);
    sb.add_end_of_month(false);
    auto schedule = sb.Finish();

    quantra::CDSBuilder cdsb(b);
    cdsb.add_side(c.fbSide);
    cdsb.add_notional(c.notional);
    cdsb.add_running_coupon(c.runningCoupon);
    cdsb.add_schedule(schedule);
    cdsb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_business_day_convention(quantra::enums::BusinessDayConvention_Following);
    cdsb.add_settles_accrual(true);
    cdsb.add_pays_at_default_time(true);
    cdsb.add_rebates_accrual(true);
    cdsb.add_last_period_day_counter(quantra::enums::DayCounter_Actual360);
    cdsb.add_cash_settlement_days(3);
    auto cds = cdsb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceCDSBuilder pcdsb(b);
    pcdsb.add_cds(cds);
    pcdsb.add_discounting_curve(dc);
    pcdsb.add_credit_curve_id(credit_id);
    pcdsb.add_model(model_id);
    auto cdss = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceCDS>>{pcdsb.Finish()});

    quantra::PriceCDSRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_cds_list(cdss);
    b.Finish(rb.Finish());

    CDSPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceCDSRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceCDSResponse>(respB->GetBufferPointer())->cds_list()->Get(0);
    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlFair, r->fair_spread().value(), 1e-6);
}

}} // namespace quantra::testing
