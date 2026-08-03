// Swaption parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, Swaption_NPVMatches) {
    std::cout << "\n=== Swaption ===" << std::endl;
    double notional = 1000000.0, strike = 0.035, vol = 0.20;
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);
    
    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed()));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BlackSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;
    
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    
    auto volSurface = buildSwaptionVolSurface(b, "swaption_vol", vol);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    
    auto model = buildSwaptionModel(b, "black_swaption_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    
    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, vols, models);
    
    // Fixed leg schedule
    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();
    
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();
    
    // Float leg schedule
    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();
    
    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();
    
    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_vol");
    auto model_id = b.CreateString("black_swaption_model");
    
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});
    
    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());
    
    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

TEST_F(QuantraComparisonTest, Swaption_Bermudan_HullWhiteLattice_NPVMatches) {
    std::cout << "\n=== Swaption Bermudan (Hull-White Lattice) ===" << std::endl;
    double notional = 1000000.0, strike = 0.035, vol = 0.20;
    const double hwA = 0.03;
    const double hwSigma = 0.01;
    const int latticeSteps = 50;
    QuantLib::Date swapStart(17, QuantLib::January, 2026);
    QuantLib::Date swapEnd(17, QuantLib::January, 2031);

    std::vector<QuantLib::Date> exerciseDates = {
        QuantLib::Date(15, QuantLib::January, 2026),
        QuantLib::Date(15, QuantLib::January, 2027),
        QuantLib::Date(15, QuantLib::January, 2028)
    };

    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::BermudanExercise>(exerciseDates);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto hwModel = std::make_shared<QuantLib::HullWhite>(discountHandle_, hwA, hwSigma);
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::TreeSwaptionEngine>(hwModel, latticeSteps));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;

    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    auto volSurface = buildSwaptionVolSurface(b, "swaption_vol", vol);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(
        b, "hw_lattice_model", quantra::enums::IrModelType_HullWhiteLattice, hwA, hwSigma, latticeSteps);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, vols, models);

    // Fixed leg schedule
    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    // Float leg schedule
    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    std::vector<flatbuffers::Offset<flatbuffers::String>> exDateStrs = {
        b.CreateString("2026-01-15"),
        b.CreateString("2027-01-15"),
        b.CreateString("2028-01-15")
    };
    auto exDatesVec = b.CreateVector(exDateStrs);

    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_type(quantra::enums::ExerciseType_Bermudan);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    swb.add_exercise_dates(exDatesVec);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_vol");
    auto model_id = b.CreateString("hw_lattice_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.05);
}

TEST_F(QuantraComparisonTest, PriceSwaption_InlineCalibrate_MatchesEndpoint) {
    const double notional = 1000000.0;
    const double strike = 0.035;

    auto buildSwaptionPricing = [&](flatbuffers::grpc::MessageBuilder& b, bool calibrateMode, double hwA, double hwSigma, const std::string& modelId) {
        auto ts = buildCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
        std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years) };
        auto volSurface = buildSwaptionVolAtmMatrixSurface(
            b, "swaption_atm", expiries, tenors, {0.20, 0.21, 0.22, 0.23});
        auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
        auto model = buildSwaptionModel(
            b, modelId, quantra::enums::IrModelType_HullWhiteLattice, hwA, hwSigma, 50,
            calibrateMode ? quantra::enums::ModelParamMode_Calibrate : quantra::enums::ModelParamMode_Explicit,
            "swaption_atm", "discount", "EUR_SWAP_6M", "discount");
        auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
        auto indices = buildIndicesVector(b);
        auto swapIndices = buildSwapIndicesVector(b);
        auto asof = b.CreateString("2025-01-15");
        return buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);
    };

    auto buildBermudanTrade = [&](flatbuffers::grpc::MessageBuilder& b, const std::string& modelId) {
        auto feff = b.CreateString("2026-01-17");
        auto fterm = b.CreateString("2031-01-17");
        quantra::ScheduleBuilder fsb(b);
        fsb.add_effective_date(feff);
        fsb.add_termination_date(fterm);
        fsb.add_calendar(quantra::enums::Calendar_TARGET);
        fsb.add_frequency(quantra::enums::Frequency_Annual);
        fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        fsb.add_end_of_month(false);
        auto fixedSch = fsb.Finish();

        quantra::SwapFixedLegBuilder flb(b);
        flb.add_notional(notional);
        flb.add_schedule(fixedSch);
        flb.add_rate(strike);
        flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
        flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        auto fixedLeg = flb.Finish();

        auto fleff = b.CreateString("2026-01-17");
        auto flterm = b.CreateString("2031-01-17");
        quantra::ScheduleBuilder flsb(b);
        flsb.add_effective_date(fleff);
        flsb.add_termination_date(flterm);
        flsb.add_calendar(quantra::enums::Calendar_TARGET);
        flsb.add_frequency(quantra::enums::Frequency_Semiannual);
        flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        flsb.add_end_of_month(false);
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
        auto uswap = vsb.Finish();

        std::vector<flatbuffers::Offset<flatbuffers::String>> exDateStrs = {
            b.CreateString("2026-01-15"),
            b.CreateString("2027-01-15"),
            b.CreateString("2028-01-15")
        };
        auto exDatesVec = b.CreateVector(exDateStrs);

        quantra::SwaptionBuilder swb(b);
        swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
        swb.add_underlying(uswap.Union());
        swb.add_exercise_type(quantra::enums::ExerciseType_Bermudan);
        swb.add_settlement_type(quantra::enums::SettlementType_Physical);
        swb.add_exercise_dates(exDatesVec);
        auto swaption = swb.Finish();

        auto dc = b.CreateString("discount");
        auto vol_id = b.CreateString("swaption_atm");
        auto model_id = b.CreateString(modelId);
        quantra::PriceSwaptionBuilder psb(b);
        psb.add_swaption(swaption);
        psb.add_discounting_curve(dc);
        psb.add_forwarding_curve(dc);
        psb.add_volatility(vol_id);
        psb.add_model(model_id);
        return psb.Finish();
    };

    // Endpoint calibration first
    double aStar = 0.0;
    double sigmaStar = 0.0;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildSwaptionPricing(b, true, 0.03, 0.01, "hw_inline_model");
        auto modelId = b.CreateString("hw_inline_model");
        quantra::CalibrateSwaptionModelRequestBuilder cb(b);
        cb.add_pricing(pricing);
        cb.add_model_id(modelId);
        b.Finish(cb.Finish());

        CalibrateSwaptionModelPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
        respB->Finish(resp);
        auto out = flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(respB->GetBufferPointer());
        aStar = out->hw_a();
        sigmaStar = out->hw_sigma();
    }

    // Inline calibration pricing
    double npvInline = 0.0;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildSwaptionPricing(b, true, 0.03, 0.01, "hw_inline_model");
        auto ps = buildBermudanTrade(b, "hw_inline_model");
        auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{ps});
        quantra::PriceSwaptionRequestBuilder rb(b);
        rb.add_pricing(pricing);
        rb.add_swaptions(swaptions);
        b.Finish(rb.Finish());

        SwaptionPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
        respB->Finish(resp);
        npvInline = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();
    }

    // Explicit pricing with calibrated params
    double npvExplicit = 0.0;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto pricing = buildSwaptionPricing(b, false, aStar, sigmaStar, "hw_explicit_model");
        auto ps = buildBermudanTrade(b, "hw_explicit_model");
        auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{ps});
        quantra::PriceSwaptionRequestBuilder rb(b);
        rb.add_pricing(pricing);
        rb.add_swaptions(swaptions);
        b.Finish(rb.Finish());

        SwaptionPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
        respB->Finish(resp);
        npvExplicit = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();
    }

    EXPECT_NEAR(npvInline, npvExplicit, 1.0e-8);
}

TEST_F(QuantraComparisonTest, PriceSwaption_InlineCalibrate_CachesPerModel) {
    quantra::resetHwCalibrationCallCount();

    const double notional = 1000000.0;
    const double strike = 0.035;

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years) };
    auto volSurface = buildSwaptionVolAtmMatrixSurface(
        b, "swaption_atm", expiries, tenors, {0.20, 0.21, 0.22, 0.23});
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(
        b, "hw_inline_model", quantra::enums::IrModelType_HullWhiteLattice, 0.03, 0.01, 50,
        quantra::enums::ModelParamMode_Calibrate, "swaption_atm", "discount", "EUR_SWAP_6M", "discount");
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    std::vector<flatbuffers::Offset<flatbuffers::String>> exDateStrs = {
        b.CreateString("2026-01-15"),
        b.CreateString("2027-01-15")
    };
    auto exDatesVec = b.CreateVector(exDateStrs);

    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_type(quantra::enums::ExerciseType_Bermudan);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    swb.add_exercise_dates(exDatesVec);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_atm");
    auto model_id = b.CreateString("hw_inline_model");
    quantra::PriceSwaptionBuilder psb1(b);
    psb1.add_swaption(swaption);
    psb1.add_discounting_curve(dc);
    psb1.add_forwarding_curve(dc);
    psb1.add_volatility(vol_id);
    psb1.add_model(model_id);
    auto ps1 = psb1.Finish();

    quantra::PriceSwaptionBuilder psb2(b);
    psb2.add_swaption(swaption);
    psb2.add_discounting_curve(dc);
    psb2.add_forwarding_curve(dc);
    psb2.add_volatility(vol_id);
    psb2.add_model(model_id);
    auto ps2 = psb2.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{ps1, ps2});
    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);

    EXPECT_EQ(quantra::getHwCalibrationCallCount(), 1);
}

TEST_F(QuantraComparisonTest, Swaption_ATMMatrix_NPVMatches) {
    std::cout << "\n=== Swaption (ATM Matrix) ===" << std::endl;
    double notional = 1000000.0, strike = 0.035;
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);

    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);

    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years), QuantLib::Period(10, QuantLib::Years) };
    QuantLib::Matrix qlVols(2, 2);
    qlVols[0][0] = 0.20; qlVols[0][1] = 0.22;
    qlVols[1][0] = 0.24; qlVols[1][1] = 0.25;
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::SwaptionVolatilityMatrix>(
            evaluationDate_, QuantLib::TARGET(), QuantLib::ModifiedFollowing,
            expiries, tenors, qlVols, QuantLib::Actual365Fixed(), false, QuantLib::ShiftedLognormal, QuantLib::Matrix()));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BlackSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionVolAtmMatrixSurface(
        b, "swaption_atm", expiries, tenors, {0.20, 0.22, 0.24, 0.25});
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "black_swaption_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_atm");
    auto model_id = b.CreateString("black_swaption_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto res = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0);
    double qNPV = res->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_EQ(res->vol_kind(), quantra::enums::SwaptionVolKind_AtmMatrix2D);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_ConstantMatches) {
    std::cout << "\n=== Swaption (Smile Cube) ===" << std::endl;
    double notional = 1000000.0, strike = 0.02, vol = 0.20;
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);

    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed()));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BlackSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), vol);

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "black_swaption_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_smile");
    auto model_id = b.CreateString("black_swaption_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto res = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0);
    double qNPV = res->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
    EXPECT_EQ(res->vol_kind(), quantra::enums::SwaptionVolKind_SmileCube3D);
    EXPECT_GT(res->used_atm_forward(), 0.0);
    EXPECT_EQ(res->used_strike_kind(), quantra::enums::SwaptionStrikeKind_SpreadFromATM);
    EXPECT_NEAR(
        res->used_strike(),
        res->used_cube_node_atm() + res->used_spread_from_atm(),
        1.0e-12);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_IndexMismatchThrows) {
    double notional = 1000000.0, strike = 0.02, vol = 0.20;
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), vol);

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_3M");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "black_swaption_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b, true);
    auto swapIndices = buildSwapIndicesVector(b, true, true, true);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_smile");
    auto model_id = b.CreateString("black_swaption_model");
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_ExternalAtmRequiresFlag) {
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), 0.20);
    std::vector<double> atmForwards = { 0.02 };

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M", atmForwards, false);
    b.Finish(volSurface);

    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()),
            nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_SpreadRequiresAtmSourceAtParseTime) {
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), 0.20);

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "", {}, false);
    b.Finish(volSurface);

    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()),
            nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_ExternalAtmEqualsInjectedServerAtm) {
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat = {0.20, 0.21, 0.22};
    std::vector<double> atmForwards = {0.02};

    flatbuffers::grpc::MessageBuilder b1;
    auto volSurface1 = buildSwaptionVolSmileCubeSurface(
        b1, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M");
    b1.Finish(volSurface1);
    auto entryNoAtm = quantra::parseSwaptionVol(
        flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b1.GetBufferPointer()),
        nullptr);
    auto injected = quantra::withSwaptionSmileCubeAtm(entryNoAtm, atmForwards);

    flatbuffers::grpc::MessageBuilder b2;
    auto volSurface2 = buildSwaptionVolSmileCubeSurface(
        b2, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M", atmForwards, true);
    b2.Finish(volSurface2);
    auto external = quantra::parseSwaptionVol(
        flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b2.GetBufferPointer()),
        nullptr);

    const QuantLib::Date ref(15, QuantLib::January, 2025);
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    const QuantLib::Calendar cal = QuantLib::TARGET();
    const QuantLib::BusinessDayConvention bdc = QuantLib::ModifiedFollowing;
    const double optionTime = dc.yearFraction(ref, cal.advance(ref, expiries[0], bdc));
    const double swapLength = dc.yearFraction(ref, cal.advance(ref, tenors[0], bdc));
    const double absStrike = atmForwards[0] + strikes[1];

    double vInjected = injected.handle->volatility(optionTime, swapLength, absStrike);
    double vExternal = external.handle->volatility(optionTime, swapLength, absStrike);
    EXPECT_NEAR(vInjected, vExternal, 1.0e-12);
}

TEST_F(QuantraComparisonTest, Swaption_SmileCube_AbsoluteRejectsAtmForwards) {
    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { 0.01, 0.02, 0.03 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), 0.20);
    std::vector<double> atmForwards = { 0.02 };

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swaption_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_Absolute, "EUR_SWAP_6M", atmForwards, true);
    b.Finish(volSurface);

    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()),
            nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_ParsePopulatesEntry) {
    SabrSyntheticGrid g;

    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrParamsSurface(
        b, "sabr_params", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    b.Finish(vs);

    auto entry = quantra::parseSwaptionVol(
        flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr);

    EXPECT_EQ(entry.volKind, quantra::enums::SwaptionVolKind_SabrParams);
    EXPECT_EQ(entry.strikeKind, quantra::enums::SwaptionStrikeKind_Absolute);
    EXPECT_EQ(entry.swapIndexId, "EUR_SWAP_6M");
    EXPECT_EQ(entry.nExp, 2);
    EXPECT_EQ(entry.nTen, 2);
    ASSERT_EQ(entry.expiries.size(), 2u);
    ASSERT_EQ(entry.tenors.size(), 2u);
    ASSERT_EQ(entry.sabrAlpha.size(), 4u);
    ASSERT_EQ(entry.sabrBeta.size(), 4u);
    ASSERT_EQ(entry.sabrRho.size(), 4u);
    ASSERT_EQ(entry.sabrNu.size(), 4u);
    for (size_t i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(entry.sabrAlpha[i], g.alpha[i]);
        EXPECT_DOUBLE_EQ(entry.sabrBeta[i], g.beta[i]);
        EXPECT_DOUBLE_EQ(entry.sabrRho[i], g.rho[i]);
        EXPECT_DOUBLE_EQ(entry.sabrNu[i], g.nu[i]);
    }
    // Handle is intentionally deferred until finalize injects forwards.
    EXPECT_TRUE(entry.handle.empty());
    EXPECT_TRUE(entry.atmForwardsFlat.empty());
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_RejectsDimensionMismatch) {
    SabrSyntheticGrid g;
    // Alpha grid claims 2x2 but actually holds only 3 values.
    std::vector<double> badAlpha{0.020, 0.022, 0.023};

    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrParamsSurface(
        b, "sabr_params_bad", g.expiries, g.tenors, badAlpha, g.beta, g.rho, g.nu);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);

    // Matrix dimension override mismatches the periods grid.
    flatbuffers::grpc::MessageBuilder b2;
    auto vs2 = buildSwaptionSabrParamsSurface(
        b2, "sabr_params_bad2", g.expiries, g.tenors,
        g.alpha, g.beta, g.rho, g.nu, "EUR_SWAP_6M",
        quantra::enums::VolatilityType_Lognormal, 0.0, "2025-01-15",
        /*matrixRowsOverride=*/3, /*matrixColsOverride=*/2);
    b2.Finish(vs2);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b2.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_RejectsInvalidParameterRanges) {
    SabrSyntheticGrid g;

    auto expectThrow = [&](const std::vector<double>& alpha,
                           const std::vector<double>& beta,
                           const std::vector<double>& rho,
                           const std::vector<double>& nu,
                           const std::string& tag) {
        flatbuffers::grpc::MessageBuilder b;
        auto vs = buildSwaptionSabrParamsSurface(
            b, "sabr_invalid_" + tag, g.expiries, g.tenors, alpha, beta, rho, nu);
        b.Finish(vs);
        EXPECT_THROW(
            quantra::parseSwaptionVol(
                flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
            QuantraError) << tag;
    };

    auto badAlpha = g.alpha; badAlpha[0] = 0.0;       // alpha must be > 0
    expectThrow(badAlpha, g.beta, g.rho, g.nu, "alpha_zero");

    auto badAlpha2 = g.alpha; badAlpha2[1] = -0.01;
    expectThrow(badAlpha2, g.beta, g.rho, g.nu, "alpha_negative");

    auto badBeta = g.beta; badBeta[2] = 1.5;          // beta must be in [0, 1]
    expectThrow(g.alpha, badBeta, g.rho, g.nu, "beta_above");

    auto badBeta2 = g.beta; badBeta2[3] = -0.01;
    expectThrow(g.alpha, badBeta2, g.rho, g.nu, "beta_below");

    auto badRhoLow = g.rho; badRhoLow[0] = -1.0;      // rho must be in (-1, 1)
    expectThrow(g.alpha, g.beta, badRhoLow, g.nu, "rho_low");

    auto badRhoHigh = g.rho; badRhoHigh[1] = 1.0;
    expectThrow(g.alpha, g.beta, badRhoHigh, g.nu, "rho_high");

    auto badNu = g.nu; badNu[2] = 0.0;                // nu must be > 0
    expectThrow(g.alpha, g.beta, g.rho, badNu, "nu_zero");

    auto badNu2 = g.nu; badNu2[3] = -0.01;
    expectThrow(g.alpha, g.beta, g.rho, badNu2, "nu_negative");
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_RejectsNormalVolType) {
    SabrSyntheticGrid g;
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrParamsSurface(
        b, "sabr_normal", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu,
        "EUR_SWAP_6M", quantra::enums::VolatilityType_Normal, 0.0);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_PriceWithBlackProducesFinitePositiveNpv) {
    SabrSyntheticGrid g;
    const double notional = 1000000.0;
    const double strike = 0.04;

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionSabrParamsSurface(
        b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "black_model", quantra::enums::IrModelType_Black);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("sabr_vol");
    auto model_id = b.CreateString("black_model");
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto res = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0);
    EXPECT_EQ(res->vol_kind(), quantra::enums::SwaptionVolKind_SabrParams);
    EXPECT_TRUE(std::isfinite(res->npv()));
    EXPECT_GT(res->npv(), 0.0);
}

TEST_F(QuantraComparisonTest, Swaption_SabrParams_BachelierEnginePairingRejected) {
    SabrSyntheticGrid g;
    const double notional = 1000000.0;
    const double strike = 0.04;

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionSabrParamsSurface(
        b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "bachelier_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("sabr_vol");
    auto model_id = b.CreateString("bachelier_model");
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RoundTripRecoversParameters) {
    // Correctness anchor: feed exact SABR-formula vols into the calibrate
    // path; LM should recover the original parameters near-exactly.
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};

    // ---- Phase 1: finalize a SabrParams surface to obtain per-node ATM
    // forwards from the production code path. The same forwards will be
    // used by the calibrate finalize, guaranteeing the synthetic vols line
    // up with the calibrator's strike axis.
    std::vector<double> forwards;
    std::vector<QuantLib::Real> tte;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto curve = buildLongCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        auto paramsSurface = buildSwaptionSabrParamsSurface(
            b, "sabr_params_seed", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
        auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{paramsSurface});
        auto indices = buildIndicesVector(b);
        auto swapIndices = buildSwapIndicesVector(b);
        auto asof = b.CreateString("2025-01-15");
        auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
        b.Finish(pricing);

        QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
        auto reg = quantra::PricingRegistryBuilder().build(
            flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer()));
        auto& entry = reg.volatility.swaptionVols.at("sabr_params_seed");
        auto dCurve = reg.rates.curves.at("discount");
        QuantLib::Handle<QuantLib::YieldTermStructure> dHandle(dCurve->currentLink());
        auto finalEntry = quantra::finalizeSwaptionVolEntryForPricing(
            entry, nullptr, reg, dHandle, dHandle, false, "discount", "discount");
        forwards = finalEntry.atmForwardsFlat;
        ASSERT_EQ(forwards.size(), 4u);
        tte.resize(4);
        for (int i = 0; i < 2; ++i) {
            QuantLib::Date exercise = finalEntry.calendar.advance(
                finalEntry.referenceDate, finalEntry.expiries[i],
                finalEntry.businessDayConvention);
            double t = finalEntry.dayCounter.yearFraction(finalEntry.referenceDate, exercise);
            tte[i * 2 + 0] = t;
            tte[i * 2 + 1] = t;
        }
    }

    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    // ---- Phase 2: drive the synthetic vols through /calibrate-swaption-vol.
    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto calibSurface = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{calibSurface});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    auto volIdOff = b.CreateString("sabr_calibrate");
    auto discIdOff = b.CreateString("discount");
    auto fwdIdOff = b.CreateString("discount");
    quantra::CalibrateSwaptionVolRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_vol_id(volIdOff);
    rb.add_discounting_curve_id(discIdOff);
    rb.add_forwarding_curve_id(fwdIdOff);
    b.Finish(rb.Finish());

    CalibrateSwaptionVolPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto respOff = handler.request(
        outBuilder,
        flatbuffers::GetRoot<quantra::CalibrateSwaptionVolRequest>(b.GetBufferPointer()));
    outBuilder->Finish(respOff);
    auto resp = flatbuffers::GetRoot<quantra::CalibrateSwaptionVolResponse>(
        outBuilder->GetBufferPointer());
    ASSERT_NE(resp, nullptr);
    ASSERT_NE(resp->diagnostics(), nullptr);
    auto* diag = resp->diagnostics();
    ASSERT_NE(diag->alpha_per_node(), nullptr);
    ASSERT_EQ(diag->alpha_per_node()->size(), 4u);

    // Tolerances: alpha/rho/nu within 1e-4 absolute on noiseless Hagan data;
    // beta exactly equal because beta_fixed=true clamps it during the fit.
    for (int k = 0; k < 4; ++k) {
        EXPECT_NEAR(diag->alpha_per_node()->Get(k), g.alpha[k], 1e-4) << "alpha node " << k;
        EXPECT_DOUBLE_EQ(diag->beta_per_node()->Get(k), g.beta[k]) << "beta node " << k;
        EXPECT_NEAR(diag->rho_per_node()->Get(k), g.rho[k], 1e-4) << "rho node " << k;
        EXPECT_NEAR(diag->nu_per_node()->Get(k), g.nu[k], 1e-4) << "nu node " << k;
    }

    auto* calib = diag->calibration();
    ASSERT_NE(calib, nullptr);
    EXPECT_TRUE(calib->converged());
    EXPECT_LT(calib->overall_rmse(), 1e-6);
    ASSERT_EQ(calib->per_node_rmse()->size(), 4u);
    for (int k = 0; k < 4; ++k) {
        EXPECT_LT(calib->per_node_rmse()->Get(k), 1e-6) << "rmse node " << k;
    }

    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsMismatchedDimensions) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const std::vector<double> okVols(2 * 2 * 5, 0.20);

    // Tensor n_1/n_2/n_3 dims declared incorrectly relative to expiries/tenors/strikes.
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_dim_bad", g.expiries, g.tenors, spreads, okVols,
        true, 0.5, false, "EUR_SWAP_6M",
        quantra::enums::VolatilityType_Lognormal, 0.0, "2025-01-15",
        /*n1Override=*/3, /*n2Override=*/2, /*n3Override=*/5);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);

    // Strike count too small for beta_fixed=true (needs >=3) — also rejected.
    flatbuffers::grpc::MessageBuilder b2;
    const std::vector<double> shortSpreads{0.0, 0.01};
    const std::vector<double> shortVols(2 * 2 * 2, 0.20);
    auto vs2 = buildSwaptionSabrCalibrateSurface(
        b2, "sabr_calibrate_strikes_short", g.expiries, g.tenors, shortSpreads, shortVols);
    b2.Finish(vs2);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b2.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsNonPositiveVols) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> badVols(2 * 2 * 5, 0.20);
    badVols[7] = 0.0;
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_zero_vol", g.expiries, g.tenors, spreads, badVols);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);

    std::vector<double> negVols(2 * 2 * 5, 0.20);
    negVols[3] = -0.01;
    flatbuffers::grpc::MessageBuilder b2;
    auto vs2 = buildSwaptionSabrCalibrateSurface(
        b2, "sabr_calibrate_neg_vol", g.expiries, g.tenors, spreads, negVols);
    b2.Finish(vs2);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b2.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsNormalVolType) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> okVols(2 * 2 * 5, 0.01); // normal-scale, but rejection happens before vol shape check
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_normal", g.expiries, g.tenors, spreads, okVols,
        true, 0.5, false, "EUR_SWAP_6M",
        quantra::enums::VolatilityType_Normal, 0.0);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsOisSwapIndex) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const std::vector<double> okVols(2 * 2 * 5, 0.20);

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    // Indices vector includes EUR_6M plus USD_SOFR (the OIS underlying).
    std::vector<flatbuffers::Offset<quantra::IndexDef>> idxDefs;
    idxDefs.push_back(buildIndexDef_EUR6M(b));
    idxDefs.push_back(buildIndexDef_USD_SOFR(b));
    auto indices = b.CreateVector(idxDefs);
    auto swapIndices = buildSwapIndicesVector(b, /*includeEur6m=*/true, /*includeOis=*/true);
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_ois", g.expiries, g.tenors, spreads, okVols,
        true, 0.5, false, /*swapIndexId=*/"USD_SOFR_OIS");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
    b.Finish(pricing);

    // Rejection lives at registry-build time (vol-kind cross-check against
    // swap_indices), not at parseSwaptionVol time.
    EXPECT_THROW(
        quantra::PricingRegistryBuilder().build(
            flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer())),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsTooSmallGrid) {
    // Single expiry violates the >= 2x2 minimum required by QuantLib's cube
    // interpolators.
    std::vector<QuantLib::Period> oneExp{QuantLib::Period(1, QuantLib::Years)};
    std::vector<QuantLib::Period> tenors{
        QuantLib::Period(5, QuantLib::Years), QuantLib::Period(10, QuantLib::Years)};
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const std::vector<double> vols(1 * 2 * 5, 0.20);
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_thin", oneExp, tenors, spreads, vols);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_RejectsNonEmptyWeights) {
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const std::vector<double> okVols(2 * 2 * 5, 0.20);
    flatbuffers::grpc::MessageBuilder b;
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_weights", g.expiries, g.tenors, spreads, okVols,
        true, 0.5, false, "EUR_SWAP_6M",
        quantra::enums::VolatilityType_Lognormal, 0.0, "2025-01-15",
        -1, -1, -1, /*addNonEmptyWeights=*/true);
    b.Finish(vs);
    EXPECT_THROW(
        quantra::parseSwaptionVol(
            flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr),
        QuantraError);
}

// RAII: force the SABR-calibrate cache enabled for the duration of a scope,
// restoring whatever override was in effect before on exit. The cache defaults
// OFF (QUANTRA_SABR_CACHE_ENABLED), so the cache-behavior test must turn it on
// for real to observe store/reuse; this guard keeps that enable from leaking
// into any other test.
namespace {
struct ScopedSabrCacheEnabled {
    std::optional<bool> prior;
    ScopedSabrCacheEnabled()
        : prior(quantra::SabrCalibrateCache::enabledOverrideForTesting()) {
        quantra::SabrCalibrateCache::setEnabledOverrideForTesting(true);
    }
    ~ScopedSabrCacheEnabled() {
        quantra::SabrCalibrateCache::setEnabledOverrideForTesting(prior);
    }
};
} // namespace

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_CacheBehavior) {
    // Direct cache-state observability: clear, populate via finalize with a
    // synthetic key, observe size() transitions, confirm key sensitivity.
    // The cache is default-OFF, so enable it for real for this test (scoped,
    // restored on exit) — the size-grows assertions below exercise the live
    // store/reuse path, not a relaxed check.
    ScopedSabrCacheEnabled sabrCacheOn;
    SabrCalibrateCache::instance().clear();
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 0u);

    // Build a SwaptionVolEntry and a swap-index base via the production path,
    // then call withSwaptionSabrCalibrateAtm twice with the same explicit key.
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    // Use synthetic vols so the calibrator has a clean target.
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_cache", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
    b.Finish(pricing);

    QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
    auto reg = quantra::PricingRegistryBuilder().build(
        flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer()));
    auto entry = reg.volatility.swaptionVols.at("sabr_calibrate_cache");
    auto dCurve = reg.rates.curves.at("discount");
    QuantLib::Handle<QuantLib::YieldTermStructure> dHandle(dCurve->currentLink());

    // Compute the same forwards the production path would, by hand.
    auto& sidx = reg.rates.swapIndices.get(entry.swapIndexId);
    auto computedForwards = quantra::computeServerAtmForwards(
        entry, sidx, reg.rates.indices, dHandle, dHandle);
    auto swapIndexBase = reg.rates.swapIndices.getIborSwapIndexWithCurves(
        entry.swapIndexId, entry.tenors.front(), reg.rates.indices, dHandle, dHandle);

    // First call: MISS + PUT, size 0 -> 1.
    auto out1 = quantra::withSwaptionSabrCalibrateAtm(
        entry, computedForwards, swapIndexBase, "test-key-A");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 1u);
    EXPECT_EQ(out1.sabrAlpha.size(), 4u);

    // Second call same key: HIT, size still 1.
    auto out2 = quantra::withSwaptionSabrCalibrateAtm(
        entry, computedForwards, swapIndexBase, "test-key-A");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 1u);
    // Cache HIT must return the same calibrated parameters.
    ASSERT_EQ(out2.sabrAlpha.size(), out1.sabrAlpha.size());
    for (size_t k = 0; k < out1.sabrAlpha.size(); ++k) {
        EXPECT_DOUBLE_EQ(out2.sabrAlpha[k], out1.sabrAlpha[k]);
        EXPECT_DOUBLE_EQ(out2.sabrRho[k], out1.sabrRho[k]);
        EXPECT_DOUBLE_EQ(out2.sabrNu[k], out1.sabrNu[k]);
    }

    // Third call different key: MISS + PUT, size grows to 2.
    auto out3 = quantra::withSwaptionSabrCalibrateAtm(
        entry, computedForwards, swapIndexBase, "test-key-B");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 2u);
    EXPECT_EQ(out3.sabrAlpha.size(), 4u);

    // Empty cache key bypasses cache entirely: size unchanged.
    auto out4 = quantra::withSwaptionSabrCalibrateAtm(
        entry, computedForwards, swapIndexBase, "");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 2u);
    EXPECT_EQ(out4.sabrAlpha.size(), 4u);

    // Key sensitivity: perturb one market-vol cell by 1bp and confirm the
    // canonical cache key changes. Same for a different curve cache key.
    auto entryPerturbed = entry;
    entryPerturbed.sabrMarketVolsFlat[2] += 0.0001;
    const std::string keyA = quantra::buildSabrCalibrateCacheKey(
        entry, computedForwards, "disc-key-1", "fwd-key-1");
    const std::string keyAPert = quantra::buildSabrCalibrateCacheKey(
        entryPerturbed, computedForwards, "disc-key-1", "fwd-key-1");
    EXPECT_NE(keyA, keyAPert);
    const std::string keyB = quantra::buildSabrCalibrateCacheKey(
        entry, computedForwards, "disc-key-2", "fwd-key-1");
    EXPECT_NE(keyA, keyB);

    // Bypass-when-curve-cache-disabled: finalize via the production path
    // with curve cache disabled (default in tests). Cache size stays put
    // because cube key is empty.
    const size_t sizeBefore = SabrCalibrateCache::instance().size();
    auto finalEntry = quantra::finalizeSwaptionVolEntryForPricing(
        entry, nullptr, reg, dHandle, dHandle, false, "discount", "discount");
    EXPECT_EQ(SabrCalibrateCache::instance().size(), sizeBefore);
    EXPECT_EQ(finalEntry.sabrAlpha.size(), 4u);

    SabrCalibrateCache::instance().clear();
    EXPECT_EQ(SabrCalibrateCache::instance().size(), 0u);
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_PriceWithBlackEqualsParamsPath) {
    // Architectural invariant: SabrCalibrate and SabrParams converge into the
    // same SwaptionVolEntry post-finalize, so pricing the same swaption
    // through either surface (with the same parameters) must produce the
    // same NPV within tolerance.
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    const double notional = 1000000.0;
    const double strike = 0.04;

    // ---- Pre-finalize SabrParams to get authoritative forwards.
    std::vector<double> forwards;
    std::vector<QuantLib::Real> tte;
    {
        flatbuffers::grpc::MessageBuilder b;
        auto curve = buildLongCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        auto paramsSurface = buildSwaptionSabrParamsSurface(
            b, "sabr_params_seed", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
        auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{paramsSurface});
        auto indices = buildIndicesVector(b);
        auto swapIndices = buildSwapIndicesVector(b);
        auto asof = b.CreateString("2025-01-15");
        auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);
        b.Finish(pricing);

        QuantLib::Settings::instance().evaluationDate() = evaluationDate_;
        auto reg = quantra::PricingRegistryBuilder().build(
            flatbuffers::GetRoot<quantra::Pricing>(b.GetBufferPointer()));
        auto& entry = reg.volatility.swaptionVols.at("sabr_params_seed");
        auto dCurve = reg.rates.curves.at("discount");
        QuantLib::Handle<QuantLib::YieldTermStructure> dHandle(dCurve->currentLink());
        auto finalEntry = quantra::finalizeSwaptionVolEntryForPricing(
            entry, nullptr, reg, dHandle, dHandle, false, "discount", "discount");
        forwards = finalEntry.atmForwardsFlat;
        tte.resize(4);
        for (int i = 0; i < 2; ++i) {
            QuantLib::Date exercise = finalEntry.calendar.advance(
                finalEntry.referenceDate, finalEntry.expiries[i],
                finalEntry.businessDayConvention);
            double t = finalEntry.dayCounter.yearFraction(finalEntry.referenceDate, exercise);
            tte[i * 2 + 0] = t;
            tte[i * 2 + 1] = t;
        }
    }

    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    auto buildPriceRequest = [&](bool useCalibrate) {
        auto b = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto ts = buildLongCurve(*b, "discount");
        auto curves = b->CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        flatbuffers::Offset<quantra::VolSurfaceSpec> volSurface;
        if (useCalibrate) {
            volSurface = buildSwaptionSabrCalibrateSurface(
                *b, "sabr_vol", g.expiries, g.tenors, spreads, syntheticVols);
        } else {
            volSurface = buildSwaptionSabrParamsSurface(
                *b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
        }
        auto vols = b->CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
        auto model = buildSwaptionModel(*b, "black_model", quantra::enums::IrModelType_Black);
        auto models = b->CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
        auto indices = buildIndicesVector(*b);
        auto swapIndices = buildSwapIndicesVector(*b);
        auto asof = b->CreateString("2025-01-15");
        auto pricing = buildPricing(*b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

        auto feff = b->CreateString("2026-01-17");
        auto fterm = b->CreateString("2031-01-17");
        quantra::ScheduleBuilder fsb(*b);
        fsb.add_effective_date(feff);
        fsb.add_termination_date(fterm);
        fsb.add_calendar(quantra::enums::Calendar_TARGET);
        fsb.add_frequency(quantra::enums::Frequency_Annual);
        fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        fsb.add_end_of_month(false);
        auto fixedSch = fsb.Finish();
        quantra::SwapFixedLegBuilder flb(*b);
        flb.add_notional(notional);
        flb.add_schedule(fixedSch);
        flb.add_rate(strike);
        flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
        flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        auto fixedLeg = flb.Finish();

        auto fleff = b->CreateString("2026-01-17");
        auto flterm = b->CreateString("2031-01-17");
        quantra::ScheduleBuilder flsb(*b);
        flsb.add_effective_date(fleff);
        flsb.add_termination_date(flterm);
        flsb.add_calendar(quantra::enums::Calendar_TARGET);
        flsb.add_frequency(quantra::enums::Frequency_Semiannual);
        flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
        flsb.add_end_of_month(false);
        auto floatSch = flsb.Finish();
        auto idx6m = buildIndexRef(*b, "EUR_6M");
        quantra::SwapFloatingLegBuilder flgb(*b);
        flgb.add_notional(notional);
        flgb.add_schedule(floatSch);
        flgb.add_index(idx6m);
        flgb.add_day_counter(quantra::enums::DayCounter_Actual360);
        flgb.add_spread(0.0);
        flgb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
        auto floatLeg = flgb.Finish();

        quantra::VanillaSwapBuilder vsb(*b);
        vsb.add_swap_type(quantra::enums::SwapType_Payer);
        vsb.add_fixed_leg(fixedLeg);
        vsb.add_floating_leg(floatLeg);
        auto uswap = vsb.Finish();

        auto exd = b->CreateString("2026-01-15");
        quantra::SwaptionBuilder swb(*b);
        swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
        swb.add_underlying(uswap.Union());
        swb.add_exercise_date(exd);
        swb.add_exercise_type(quantra::enums::ExerciseType_European);
        swb.add_settlement_type(quantra::enums::SettlementType_Physical);
        auto swaption = swb.Finish();

        auto dc = b->CreateString("discount");
        auto vol_id = b->CreateString("sabr_vol");
        auto model_id = b->CreateString("black_model");
        quantra::PriceSwaptionBuilder psb(*b);
        psb.add_swaption(swaption);
        psb.add_discounting_curve(dc);
        psb.add_forwarding_curve(dc);
        psb.add_volatility(vol_id);
        psb.add_model(model_id);
        auto psbOff = psb.Finish();
        auto swaptions = b->CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

        quantra::PriceSwaptionRequestBuilder rb(*b);
        rb.add_pricing(pricing);
        rb.add_swaptions(swaptions);
        b->Finish(rb.Finish());
        return b;
    };

    auto runPrice = [&](bool useCalibrate) {
        auto b = buildPriceRequest(useCalibrate);
        SwaptionPricingRequest req;
        auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(
            respB,
            flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b->GetBufferPointer()));
        respB->Finish(resp);
        return flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(
                   respB->GetBufferPointer())->swaptions()->Get(0)->npv();
    };

    double npvParams = runPrice(/*useCalibrate=*/false);
    double npvCalibrate = runPrice(/*useCalibrate=*/true);
    EXPECT_TRUE(std::isfinite(npvParams));
    EXPECT_GT(npvParams, 0.0);
    EXPECT_TRUE(std::isfinite(npvCalibrate));
    EXPECT_GT(npvCalibrate, 0.0);
    // Tolerance reflects calibrator rounding and Hagan-vs-cube interpolation
    // residual; 5bp of NPV is comfortable for noiseless data.
    EXPECT_NEAR(npvCalibrate, npvParams, std::max(1.0, npvParams * 1e-4));
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_BachelierEnginePairingRejected) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);
    const double notional = 1000000.0;
    const double strike = 0.04;

    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
    auto volSurface = buildSwaptionSabrCalibrateSurface(
        b, "sabr_vol", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "bachelier_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();
    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("sabr_vol");
    auto model_id = b.CreateString("bachelier_model");
    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();
    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    EXPECT_THROW(
        req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer())),
        QuantraError);
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, Swaption_SabrCalibrate_DiagnosticsEmitsHighRmseWarnings) {
    // Construct a deliberately non-SABR-shaped market vol cube (vol levels far
    // apart at neighbouring strikes — Hagan can't fit cleanly) and expect at
    // least one warning naming high RMSE on a node. Threshold (50bp RMSE) lives
    // in swaption_vol_diagnostics.cpp.
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    // Per-node strike-vol pattern is a symmetric W around a 20% ATM with
    // ±100 bp amplitude. SABR (Hagan, beta_fixed=0.5) cannot fit a W shape
    // — best fit is a flat/skewed smile, leaving residuals around 70-90 bp
    // per strike. That lands above the 50 bp diagnostics warning threshold
    // but below QuantLib's 1% rmsError hard throw inside
    // XabrSwaptionVolatilityCube::performCalculations, so calibration
    // completes and the warnings array is populated.
    std::vector<double> badVols;
    badVols.reserve(2 * 2 * 5);
    for (int n = 0; n < 4; ++n) {
        badVols.insert(badVols.end(), {0.21, 0.19, 0.21, 0.19, 0.21});
    }

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_warn", g.expiries, g.tenors, spreads, badVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    auto volIdOff = b.CreateString("sabr_calibrate_warn");
    auto discIdOff = b.CreateString("discount");
    auto fwdIdOff = b.CreateString("discount");
    quantra::CalibrateSwaptionVolRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_vol_id(volIdOff);
    rb.add_discounting_curve_id(discIdOff);
    rb.add_forwarding_curve_id(fwdIdOff);
    b.Finish(rb.Finish());

    CalibrateSwaptionVolPricingRequest handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto respOff = handler.request(
        outBuilder,
        flatbuffers::GetRoot<quantra::CalibrateSwaptionVolRequest>(b.GetBufferPointer()));
    outBuilder->Finish(respOff);
    auto resp = flatbuffers::GetRoot<quantra::CalibrateSwaptionVolResponse>(
        outBuilder->GetBufferPointer());
    ASSERT_NE(resp, nullptr);
    auto* d = resp->diagnostics();
    ASSERT_NE(d, nullptr);
    ASSERT_NE(d->warnings(), nullptr);
    EXPECT_GT(d->warnings()->size(), 0u)
        << "expected at least one warning on a deliberately bad SABR fit";
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, Swaption_Bachelier_NPVMatches) {
    std::cout << "\n=== Swaption (Bachelier) ===" << std::endl;
    double notional = 1000000.0, strike = 0.035, vol = 0.01; // Normal vol
    QuantLib::Date exDate = evaluationDate_ + QuantLib::Period(1, QuantLib::Years);
    QuantLib::Date swapStart = exDate + 2, swapEnd = swapStart + QuantLib::Period(5, QuantLib::Years);

    QuantLib::Schedule fixSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule fltSch(swapStart, swapEnd, QuantLib::Period(QuantLib::Semiannual), QuantLib::TARGET(),
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing, QuantLib::DateGeneration::Forward, false);
    auto idx = std::make_shared<QuantLib::Euribor6M>(forwardHandle_);
    auto swap = std::make_shared<QuantLib::VanillaSwap>(QuantLib::VanillaSwap::Payer, notional,
        fixSch, strike, QuantLib::Thirty360(QuantLib::Thirty360::BondBasis), fltSch, idx, 0.0, QuantLib::Actual360());
    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(swap, ex);
    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(evaluationDate_, QuantLib::TARGET(),
            QuantLib::ModifiedFollowing, vol, QuantLib::Actual365Fixed(), QuantLib::Normal));
    qlSwaption->setPricingEngine(std::make_shared<QuantLib::BachelierSwaptionEngine>(discountHandle_, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;

    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    auto volSurface = buildSwaptionVolSurface(
        b, "swaption_vol_norm", vol, quantra::enums::VolatilityType_Normal, 0.0, "swaption_vol_quote");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(b, "bachelier_swaption_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto indices = buildIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto quote_id = b.CreateString("swaption_vol_quote");
    quantra::QuoteSpecBuilder qb(b);
    qb.add_id(quote_id);
    qb.add_kind(quantra::QuoteKind_Rate);
    qb.add_value(vol);
    qb.add_quote_type(quantra::QuoteType_Volatility);
    auto quote = qb.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{quote});

    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, vols, models);

    // Fixed leg schedule
    auto feff = b.CreateString("2026-01-17");
    auto fterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_TARGET);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Thirty360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    // Float leg schedule
    auto fleff = b.CreateString("2026-01-17");
    auto flterm = b.CreateString("2031-01-17");
    quantra::ScheduleBuilder flsb(b);
    flsb.add_effective_date(fleff);
    flsb.add_termination_date(flterm);
    flsb.add_calendar(quantra::enums::Calendar_TARGET);
    flsb.add_frequency(quantra::enums::Frequency_Semiannual);
    flsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    flsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    flsb.add_end_of_month(false);
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
    auto uswap = vsb.Finish();

    auto exd = b.CreateString("2026-01-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_underlying_type(quantra::SwaptionUnderlying_VanillaSwap);
    swb.add_underlying(uswap.Union());
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Physical);
    auto swaption = swb.Finish();

    auto dc = b.CreateString("discount");
    auto vol_id = b.CreateString("swaption_vol_norm");
    auto model_id = b.CreateString("bachelier_swaption_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.01);
}

TEST_F(QuantraComparisonTest, Swaption_OIS_Bachelier_NPVMatches) {
    std::cout << "\n=== Swaption (OIS SOFR, Bachelier) ===" << std::endl;

    QuantLib::Date prevEval = QuantLib::Settings::instance().evaluationDate();
    QuantLib::Date evalDate(14, QuantLib::August, 2024);
    QuantLib::Settings::instance().evaluationDate() = evalDate;

    double notional = 1000000.0;
    double strike = 0.03367463;
    double vol = 0.010267; // Normal vol

    QuantLib::Date exerciseDate(16, QuantLib::September, 2024);
    QuantLib::Date swapStart(18, QuantLib::September, 2024);
    QuantLib::Date swapEnd(18, QuantLib::September, 2034);

    QuantLib::Calendar usGov = QuantLib::UnitedStates(QuantLib::UnitedStates::GovernmentBond);

    struct OisRate {
        QuantLib::Period tenor;
        double rate;
    };

    std::vector<OisRate> oisRates = {
        {1 * QuantLib::Weeks, 0.0533410},
        {2 * QuantLib::Weeks, 0.0533585},
        {3 * QuantLib::Weeks, 0.0533814},
        {1 * QuantLib::Months, 0.0534261},
        {2 * QuantLib::Months, 0.0520565},
        {3 * QuantLib::Months, 0.0511385},
        {4 * QuantLib::Months, 0.0502265},
        {5 * QuantLib::Months, 0.0490300},
        {6 * QuantLib::Months, 0.0478850},
        {7 * QuantLib::Months, 0.0470850},
        {8 * QuantLib::Months, 0.0460968},
        {9 * QuantLib::Months, 0.0452458},
        {10 * QuantLib::Months, 0.0444082},
        {11 * QuantLib::Months, 0.0436380},
        {12 * QuantLib::Months, 0.0428710},
        {18 * QuantLib::Months, 0.0392930},
        {2 * QuantLib::Years, 0.0373480},
        {3 * QuantLib::Years, 0.0351270},
        {4 * QuantLib::Years, 0.0340905},
        {5 * QuantLib::Years, 0.0336448},
        {6 * QuantLib::Years, 0.0334900},
        {7 * QuantLib::Years, 0.0334540},
        {8 * QuantLib::Years, 0.0335100},
        {9 * QuantLib::Years, 0.0336048},
        {10 * QuantLib::Years, 0.0337219},
        {12 * QuantLib::Years, 0.0340177},
        {15 * QuantLib::Years, 0.0343655},
        {20 * QuantLib::Years, 0.0343820},
        {25 * QuantLib::Years, 0.0337260},
        {30 * QuantLib::Years, 0.0329430},
        {40 * QuantLib::Years, 0.0310050},
        {50 * QuantLib::Years, 0.0290915}
    };

    std::vector<std::shared_ptr<QuantLib::RateHelper>> helpers;
    auto sofr = std::make_shared<QuantLib::OvernightIndex>(
        "SOFR", 0, QuantLib::USDCurrency(), usGov, QuantLib::Actual360());
    for (const auto& tr : oisRates) {
        helpers.push_back(std::make_shared<QuantLib::OISRateHelper>(
            2, tr.tenor, tr.rate, sofr));
    }

    auto oisCurve = std::make_shared<
        QuantLib::PiecewiseYieldCurve<QuantLib::Discount, QuantLib::LogLinear>>(
        evalDate, helpers, QuantLib::Actual365Fixed());
    oisCurve->enableExtrapolation();

    QuantLib::Handle<QuantLib::YieldTermStructure> oisHandle(oisCurve);
    auto sofrWithCurve = std::make_shared<QuantLib::OvernightIndex>(
        "SOFR", 0, QuantLib::USDCurrency(), usGov, QuantLib::Actual360(), oisHandle);

    QuantLib::Schedule fixedSchedule(
        swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), usGov,
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);
    QuantLib::Schedule floatSchedule(
        swapStart, swapEnd, QuantLib::Period(QuantLib::Annual), usGov,
        QuantLib::ModifiedFollowing, QuantLib::ModifiedFollowing,
        QuantLib::DateGeneration::Forward, false);

    auto oisSwap = std::make_shared<QuantLib::OvernightIndexedSwap>(
        QuantLib::OvernightIndexedSwap::Payer,
        notional,
        fixedSchedule,
        strike,
        QuantLib::Actual360(),
        floatSchedule,
        sofrWithCurve,
        0.0,
        2,
        QuantLib::ModifiedFollowing,
        usGov,
        false,
        QuantLib::RateAveraging::Compound);

    auto ex = std::make_shared<QuantLib::EuropeanExercise>(exerciseDate);
    auto qlSwaption = std::make_shared<QuantLib::Swaption>(
        oisSwap, ex, QuantLib::Settlement::Cash, QuantLib::Settlement::ParYieldCurve);

    auto volH = QuantLib::Handle<QuantLib::SwaptionVolatilityStructure>(
        std::make_shared<QuantLib::ConstantSwaptionVolatility>(
            evalDate, usGov, QuantLib::ModifiedFollowing, vol,
            QuantLib::Actual365Fixed(), QuantLib::Normal));
    qlSwaption->setPricingEngine(
        std::make_shared<QuantLib::BachelierSwaptionEngine>(oisHandle, volH));
    double qlNPV = qlSwaption->NPV();

    flatbuffers::grpc::MessageBuilder b;

    std::vector<OisTenorRate> oisRatesFb = {
        {1, quantra::enums::TimeUnit_Weeks, 0.0533410},
        {2, quantra::enums::TimeUnit_Weeks, 0.0533585},
        {3, quantra::enums::TimeUnit_Weeks, 0.0533814},
        {1, quantra::enums::TimeUnit_Months, 0.0534261},
        {2, quantra::enums::TimeUnit_Months, 0.0520565},
        {3, quantra::enums::TimeUnit_Months, 0.0511385},
        {4, quantra::enums::TimeUnit_Months, 0.0502265},
        {5, quantra::enums::TimeUnit_Months, 0.0490300},
        {6, quantra::enums::TimeUnit_Months, 0.0478850},
        {7, quantra::enums::TimeUnit_Months, 0.0470850},
        {8, quantra::enums::TimeUnit_Months, 0.0460968},
        {9, quantra::enums::TimeUnit_Months, 0.0452458},
        {10, quantra::enums::TimeUnit_Months, 0.0444082},
        {11, quantra::enums::TimeUnit_Months, 0.0436380},
        {12, quantra::enums::TimeUnit_Months, 0.0428710},
        {18, quantra::enums::TimeUnit_Months, 0.0392930},
        {2, quantra::enums::TimeUnit_Years, 0.0373480},
        {3, quantra::enums::TimeUnit_Years, 0.0351270},
        {4, quantra::enums::TimeUnit_Years, 0.0340905},
        {5, quantra::enums::TimeUnit_Years, 0.0336448},
        {6, quantra::enums::TimeUnit_Years, 0.0334900},
        {7, quantra::enums::TimeUnit_Years, 0.0334540},
        {8, quantra::enums::TimeUnit_Years, 0.0335100},
        {9, quantra::enums::TimeUnit_Years, 0.0336048},
        {10, quantra::enums::TimeUnit_Years, 0.0337219},
        {12, quantra::enums::TimeUnit_Years, 0.0340177},
        {15, quantra::enums::TimeUnit_Years, 0.0343655},
        {20, quantra::enums::TimeUnit_Years, 0.0343820},
        {25, quantra::enums::TimeUnit_Years, 0.0337260},
        {30, quantra::enums::TimeUnit_Years, 0.0329430},
        {40, quantra::enums::TimeUnit_Years, 0.0310050},
        {50, quantra::enums::TimeUnit_Years, 0.0290915}
    };

    auto ts = buildOisCurve(
        b, "USD_SOFR", "USD_SOFR", oisRatesFb,
        quantra::enums::Calendar_UnitedStatesGovernmentBond);
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    auto volSurface = buildSwaptionVolSurface(
        b, "usd_sofr_vol", vol, quantra::enums::VolatilityType_Normal, 0.0, "", "2024-08-14");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(b, "bachelier_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});

    auto indices = b.CreateVector(
        std::vector<flatbuffers::Offset<quantra::IndexDef>>{buildIndexDef_USD_SOFR(b)});
    auto asof = b.CreateString("2024-08-14");

    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, vols, models);

    // Fixed leg schedule
    auto feff = b.CreateString("2024-09-18");
    auto fterm = b.CreateString("2034-09-18");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(notional);
    flb.add_schedule(fixedSch);
    flb.add_rate(strike);
    flb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    // Overnight leg schedule
    auto oeff = b.CreateString("2024-09-18");
    auto oterm = b.CreateString("2034-09-18");
    quantra::ScheduleBuilder osb(b);
    osb.add_effective_date(oeff);
    osb.add_termination_date(oterm);
    osb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    osb.add_frequency(quantra::enums::Frequency_Annual);
    osb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    osb.add_end_of_month(false);
    auto overnightSch = osb.Finish();

    auto idxRef = buildIndexRef(b, "USD_SOFR");
    quantra::OisFloatingLegBuilder olb(b);
    olb.add_notional(notional);
    olb.add_schedule(overnightSch);
    olb.add_index(idxRef);
    olb.add_spread(0.0);
    olb.add_day_counter(quantra::enums::DayCounter_Actual360);
    olb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    olb.add_payment_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    olb.add_payment_lag(2);
    olb.add_averaging_method(quantra::enums::RateAveragingType_Compound);
    olb.add_lookback_days(0);
    olb.add_lockout_days(0);
    olb.add_apply_observation_shift(false);
    olb.add_telescopic_value_dates(false);
    auto overnightLeg = olb.Finish();

    quantra::OisSwapBuilder oisBuilder(b);
    oisBuilder.add_swap_type(quantra::enums::SwapType_Payer);
    oisBuilder.add_fixed_leg(fixedLeg);
    oisBuilder.add_overnight_leg(overnightLeg);
    auto oisSwapFb = oisBuilder.Finish();

    auto exd = b.CreateString("2024-09-16");
    quantra::SwaptionBuilder swb(b);
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Cash);
    swb.add_settlement_method(quantra::enums::SettlementMethod_ParYieldCurve);
    swb.add_underlying_type(quantra::SwaptionUnderlying_OisSwap);
    swb.add_underlying(oisSwapFb.Union());
    auto swaption = swb.Finish();

    auto dc = b.CreateString("USD_SOFR");
    auto vol_id = b.CreateString("usd_sofr_vol");
    auto model_id = b.CreateString("bachelier_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});

    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    double qNPV = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0)->npv();

    std::cout << "QuantLib: " << qlNPV << " | Quantra: " << qNPV << " | Diff: " << std::abs(qlNPV-qNPV) << std::endl;
    EXPECT_NEAR(qlNPV, qNPV, 0.05);

    QuantLib::Settings::instance().evaluationDate() = prevEval;
}

TEST_F(QuantraComparisonTest, Swaption_OIS_SmileCubeSpreadFromATM_UsesSwapIndexRegistry) {
    flatbuffers::grpc::MessageBuilder b;

    std::vector<OisTenorRate> oisRatesFb = {
        {1, quantra::enums::TimeUnit_Months, 0.02},
        {6, quantra::enums::TimeUnit_Months, 0.021},
        {1, quantra::enums::TimeUnit_Years, 0.022},
        {2, quantra::enums::TimeUnit_Years, 0.023},
        {5, quantra::enums::TimeUnit_Years, 0.024},
        {10, quantra::enums::TimeUnit_Years, 0.025}
    };

    auto ts = buildOisCurve(
        b, "USD_SOFR", "USD_SOFR", oisRatesFb,
        quantra::enums::Calendar_UnitedStatesGovernmentBond);
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years) };
    std::vector<double> strikes = { -0.01, 0.0, 0.01 };
    std::vector<double> volsFlat(expiries.size() * tenors.size() * strikes.size(), 0.01);
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "usd_sofr_smile", expiries, tenors, strikes, volsFlat,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "USD_SOFR_OIS",
        {}, false, quantra::enums::VolatilityType_Normal, 0.0, "2024-08-14");
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto model = buildSwaptionModel(b, "bachelier_model", quantra::enums::IrModelType_Bachelier);
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = b.CreateVector(
        std::vector<flatbuffers::Offset<quantra::IndexDef>>{buildIndexDef_USD_SOFR(b)});
    auto swapIndices = buildSwapIndicesVector(b, false, true, false);
    auto asof = b.CreateString("2024-08-14");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models, 0, 0, 0, false, false, false, true);

    auto feff = b.CreateString("2025-08-18");
    auto fterm = b.CreateString("2030-08-18");
    quantra::ScheduleBuilder fsb(b);
    fsb.add_effective_date(feff);
    fsb.add_termination_date(fterm);
    fsb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    fsb.add_frequency(quantra::enums::Frequency_Annual);
    fsb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    fsb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    fsb.add_end_of_month(false);
    auto fixedSch = fsb.Finish();

    quantra::SwapFixedLegBuilder flb(b);
    flb.add_notional(1000000.0);
    flb.add_schedule(fixedSch);
    flb.add_rate(0.025);
    flb.add_day_counter(quantra::enums::DayCounter_Actual360);
    flb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto fixedLeg = flb.Finish();

    auto oeff = b.CreateString("2025-08-18");
    auto oterm = b.CreateString("2030-08-18");
    quantra::ScheduleBuilder osb(b);
    osb.add_effective_date(oeff);
    osb.add_termination_date(oterm);
    osb.add_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    osb.add_frequency(quantra::enums::Frequency_Annual);
    osb.add_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_termination_date_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    osb.add_date_generation_rule(quantra::enums::DateGenerationRule_Forward);
    osb.add_end_of_month(false);
    auto overnightSch = osb.Finish();

    auto idxRef = buildIndexRef(b, "USD_SOFR");
    quantra::OisFloatingLegBuilder olb(b);
    olb.add_notional(1000000.0);
    olb.add_schedule(overnightSch);
    olb.add_index(idxRef);
    olb.add_spread(0.0);
    olb.add_day_counter(quantra::enums::DayCounter_Actual360);
    olb.add_payment_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    olb.add_payment_calendar(quantra::enums::Calendar_UnitedStatesGovernmentBond);
    olb.add_payment_lag(2);
    olb.add_averaging_method(quantra::enums::RateAveragingType_Compound);
    olb.add_lookback_days(0);
    olb.add_lockout_days(0);
    olb.add_apply_observation_shift(false);
    olb.add_telescopic_value_dates(false);
    auto overnightLeg = olb.Finish();

    quantra::OisSwapBuilder oisBuilder(b);
    oisBuilder.add_swap_type(quantra::enums::SwapType_Payer);
    oisBuilder.add_fixed_leg(fixedLeg);
    oisBuilder.add_overnight_leg(overnightLeg);
    auto oisSwapFb = oisBuilder.Finish();

    auto exd = b.CreateString("2025-08-15");
    quantra::SwaptionBuilder swb(b);
    swb.add_exercise_date(exd);
    swb.add_exercise_type(quantra::enums::ExerciseType_European);
    swb.add_settlement_type(quantra::enums::SettlementType_Cash);
    swb.add_settlement_method(quantra::enums::SettlementMethod_ParYieldCurve);
    swb.add_underlying_type(quantra::SwaptionUnderlying_OisSwap);
    swb.add_underlying(oisSwapFb.Union());
    auto swaption = swb.Finish();

    auto dc = b.CreateString("USD_SOFR");
    auto vol_id = b.CreateString("usd_sofr_smile");
    auto model_id = b.CreateString("bachelier_model");

    quantra::PriceSwaptionBuilder psb(b);
    psb.add_swaption(swaption);
    psb.add_discounting_curve(dc);
    psb.add_forwarding_curve(dc);
    psb.add_volatility(vol_id);
    psb.add_model(model_id);
    auto psbOff = psb.Finish();

    auto swaptions = b.CreateVector(std::vector<flatbuffers::Offset<quantra::PriceSwaption>>{psbOff});
    quantra::PriceSwaptionRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_swaptions(swaptions);
    b.Finish(rb.Finish());

    SwaptionPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::PriceSwaptionRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto res = flatbuffers::GetRoot<quantra::PriceSwaptionResponse>(respB->GetBufferPointer())->swaptions()->Get(0);

    EXPECT_EQ(res->vol_kind(), quantra::enums::SwaptionVolKind_SmileCube3D);
    EXPECT_EQ(res->used_strike_kind(), quantra::enums::SwaptionStrikeKind_SpreadFromATM);
    EXPECT_GT(res->used_atm_forward(), 0.0);
    EXPECT_GT(res->used_cube_node_atm(), 0.0);
}

}} // namespace quantra::testing
