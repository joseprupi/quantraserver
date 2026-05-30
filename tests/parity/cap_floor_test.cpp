// Cap / Floor parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, Cap_NPVMatches) {
    std::cout << "\n=== Cap ===" << std::endl;
    double notional = 1000000.0, strike = 0.04, vol = 0.20;
    QuantLib::Date start = evaluationDate_ + 2, end = start + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule sch(start, end, QuantLib::Period(QuantLib::Quarterly), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    QuantLib::Leg leg = QuantLib::IborLeg(sch, idx).withNotionals(notional)
        .withPaymentDayCounter(QuantLib::Actual360()).withPaymentAdjustment(QuantLib::ModifiedFollowing).withFixingDays(2);
    auto qlCap = std::make_shared<QuantLib::Cap>(leg, std::vector<QuantLib::Rate>(1, strike));
    auto volH = QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(
        std::make_shared<QuantLib::ConstantOptionletVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed()));
    qlCap->setPricingEngine(std::make_shared<QuantLib::BlackCapFloorEngine>(discountHandle_, volH));
    double qlNPV = qlCap->NPV();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    
    auto volSurface = buildOptionletVolSurface(b, "vol_20pct", vol);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    
    auto model = buildCapFloorModel(b, "black_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    
    auto indices = buildIndicesVector(b, true);  // include EUR_3M for cap
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, vols, models, 0, 0, 0, false, false, false, true);
    
    auto eff = b.CreateString("2025-01-17");
    auto term = b.CreateString("2030-01-17");
    quantra::ScheduleBuilder sb(b);
    sb.add_effective_date(eff);
    sb.add_termination_date(term);
    sb.add_calendar(quantra::enums::Calendar_TARGET);
    sb.add_frequency(quantra::enums::Frequency_Quarterly);
    sb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    sb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    auto schedule = sb.Finish();
    
    auto idx3m = buildIndexRef(b, "EUR_3M");
    quantra::CapFloorBuilder cb(b);
    cb.add_cap_floor_type(quantra::enums::CapFloorType_Cap);
    cb.add_notional(notional);
    cb.add_schedule(schedule);
    cb.add_strike(strike);
    cb.add_index(idx3m);
    cb.add_day_counter(quantra::enums::DayCounter_Actual360);
    cb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto cap = cb.Finish();
    
    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("vol_20pct");
    auto model_id = b.CreateString("black_model");
    
    quantra::PriceCapFloorBuilder pcb(b);
    pcb.add_cap_floor(cap);
    pcb.add_discounting_curve(dc);
    pcb.add_forwarding_curve(dc);
    pcb.add_volatility(vol_id);
    pcb.add_model(model_id);
    auto pcbOff = pcb.Finish();
    
    auto caps = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceCapFloor>>{pcbOff});
    
    quantra::PriceCapFloorRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_cap_floors(caps);
    b.Finish(rb.Finish());
    
    CapFloorPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceCapFloorRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceCapFloorResponse>(respB->GetBufferPointer())->cap_floors()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

}} // namespace quantra::testing
