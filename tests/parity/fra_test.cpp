// FRA parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, FRA_NPVMatches) {
    std::cout << "\n=== FRA ===" << std::endl;
    double notional = 1000000.0, strike = 0.032;
    QuantLib::Date valDate = evaluationDate_ + QuantLib::Period(3, QuantLib::Months);
    QuantLib::Date matDate = evaluationDate_ + QuantLib::Period(6, QuantLib::Months);
    
    auto idx = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    auto qlFRA = std::make_shared<QuantLib::ForwardRateAgreement>(
        idx, valDate, matDate, QuantLib::Position::Long, strike, notional, discountHandle_);
    double qlNPV = qlFRA->NPV();
    double qlFwd = qlFRA->forwardRate();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b, true);  // include EUR_3M
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);
    
    auto vd = b.CreateString("2025-04-15");
    auto md = b.CreateString("2025-07-15");
    auto idx3m = buildIndexRef(b, "EUR_3M");
    
    quantra::FRABuilder fb(b);
    fb.add_start_date(vd);
    fb.add_maturity_date(md);
    fb.add_fra_type(quantra::enums::FRAType_Long);
    fb.add_strike(strike);
    fb.add_notional(notional);
    fb.add_index(idx3m);
    fb.add_day_counter(quantra::enums::DayCounter_Actual360);
    fb.add_calendar(quantra::enums::Calendar_TARGET);
    fb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fra = fb.Finish();
    
    auto dc = b.CreateString("discount");
    quantra::PriceFRABuilder pfb(b);
    pfb.add_fra(fra);
    pfb.add_discounting_curve(dc);
    pfb.add_forwarding_curve(dc);
    auto pfbOff = pfb.Finish();
    
    auto fras = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFRA>>{pfbOff});
    
    quantra::PriceFRARequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_fras(fras);
    b.Finish(rb.Finish());
    
    FRAPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFRARequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto r = flatbuffers::GetRoot<quantra::PriceFRAResponse>(respB->GetBufferPointer())->fras()->Get(0);
    double qNPV = r->npv();
    double qFwd = r->forward_rate();

    std::cout << "QuantLib NPV: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    std::cout << "QuantLib Fwd: " << qlFwd*100 << "% | Quantra: " << qFwd*100 << "%" << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_NEAR(qlFwd, qFwd, 1e-6);
}

// Short position on a 3M index — flips the sign of the payoff vs the base case.
TEST_F(QuantraComparisonTest, FRA_Short_3M) {
    const double notional = 1000000.0, strike = 0.040;
    auto idx = std::make_shared<QuantLib::Euribor3M>(forwardHandle_);
    auto qlFRA = std::make_shared<QuantLib::ForwardRateAgreement>(
        idx, QuantLib::Date(15, QuantLib::April, 2025),
        QuantLib::Date(15, QuantLib::July, 2025), QuantLib::Position::Short,
        strike, notional, discountHandle_);
    const double qlNPV = qlFRA->NPV();
    const double qlFwd = qlFRA->forwardRate();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b, true);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);

    auto vd = b.CreateString("2025-04-15");
    auto md = b.CreateString("2025-07-15");
    auto idx3m = buildIndexRef(b, "EUR_3M");
    quantra::FRABuilder fb(b);
    fb.add_start_date(vd);
    fb.add_maturity_date(md);
    fb.add_fra_type(quantra::enums::FRAType_Short);
    fb.add_strike(strike);
    fb.add_notional(notional);
    fb.add_index(idx3m);
    fb.add_day_counter(quantra::enums::DayCounter_Actual360);
    fb.add_calendar(quantra::enums::Calendar_TARGET);
    fb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fra = fb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceFRABuilder pfb(b);
    pfb.add_fra(fra);
    pfb.add_discounting_curve(dc);
    pfb.add_forwarding_curve(dc);
    auto fras = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFRA>>{pfb.Finish()});

    quantra::PriceFRARequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_fras(fras);
    b.Finish(rb.Finish());

    FRAPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFRARequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceFRAResponse>(respB->GetBufferPointer())->fras()->Get(0);
    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlFwd, r->forward_rate(), 1e-6);
}

// Long position on a 6M index with a later start — exercises a different index
// tenor (Euribor6M) than the 3M base/short cases.
TEST_F(QuantraComparisonTest, FRA_Long_6M) {
    const double notional = 1000000.0, strike = 0.030;
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto qlFRA = std::make_shared<QuantLib::ForwardRateAgreement>(
        idx, QuantLib::Date(15, QuantLib::July, 2025),
        QuantLib::Date(15, QuantLib::January, 2026), QuantLib::Position::Long,
        strike, notional, discountHandle_);
    const double qlNPV = qlFRA->NPV();
    const double qlFwd = qlFRA->forwardRate();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto indices = buildIndicesVector(b, true);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, asof, 0, indices, 0, curves);

    auto vd = b.CreateString("2025-07-15");
    auto md = b.CreateString("2026-01-15");
    auto idx6m = buildIndexRef(b, "EUR_6M");
    quantra::FRABuilder fb(b);
    fb.add_start_date(vd);
    fb.add_maturity_date(md);
    fb.add_fra_type(quantra::enums::FRAType_Long);
    fb.add_strike(strike);
    fb.add_notional(notional);
    fb.add_index(idx6m);
    fb.add_day_counter(quantra::enums::DayCounter_Actual360);
    fb.add_calendar(quantra::enums::Calendar_TARGET);
    fb.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fra = fb.Finish();

    auto dc = b.CreateString("discount");
    quantra::PriceFRABuilder pfb(b);
    pfb.add_fra(fra);
    pfb.add_discounting_curve(dc);
    pfb.add_forwarding_curve(dc);
    auto fras = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceFRA>>{pfb.Finish()});

    quantra::PriceFRARequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_fras(fras);
    b.Finish(rb.Finish());

    FRAPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceFRARequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    const auto* r = flatbuffers::GetRoot<quantra::PriceFRAResponse>(respB->GetBufferPointer())->fras()->Get(0);
    EXPECT_NEAR(qlNPV, r->npv(), 0.01);
    EXPECT_NEAR(qlFwd, r->forward_rate(), 1e-6);
}

}} // namespace quantra::testing
