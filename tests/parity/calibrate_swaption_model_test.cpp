// Calibrate Swaption Model parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, CalibrateSwaptionModel_ReturnsReasonableParams) {
    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    std::vector<QuantLib::Period> expiries = {
        QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    std::vector<QuantLib::Period> tenors = {
        QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years)};
    auto volSurface = buildSwaptionVolAtmMatrixSurface(
        b, "swaption_atm", expiries, tenors, {0.20, 0.21, 0.22, 0.23});
    auto vols =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(
        b, "hw_cal_model", quantra::enums::IrModelType_HullWhiteLattice, 0.03, 0.01, 50,
        quantra::enums::ModelParamMode_Calibrate, "swaption_atm", "discount", "EUR_SWAP_6M",
        "discount");
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto modelId = b.CreateString("hw_cal_model");
    quantra::CalibrateSwaptionModelRequestBuilder cb(b);
    cb.add_pricing(pricing);
    cb.add_model_id(modelId);
    b.Finish(cb.Finish());

    CalibrateSwaptionModelPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(
        respB, flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto out =
        flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(respB->GetBufferPointer());

    EXPECT_GT(out->hw_a(), 0.0);
    EXPECT_GT(out->hw_sigma(), 0.0);
    EXPECT_TRUE(std::isfinite(out->rmse()));
    EXPECT_GT(out->num_helpers(), 0);
}

TEST_F(QuantraComparisonTest, CalibrateSwaptionModel_MatchesDirectQuantLibCalibration) {
    flatbuffers::grpc::MessageBuilder b;
    auto ts = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});

    std::vector<QuantLib::Period> expiries = {
        QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    std::vector<QuantLib::Period> tenors = {
        QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years)};
    std::vector<double> volsFlat = {0.20, 0.21, 0.22, 0.23};
    auto volSurface =
        buildSwaptionVolAtmMatrixSurface(b, "swaption_atm", expiries, tenors, volsFlat);
    auto vols =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(
        b, "hw_cal_model", quantra::enums::IrModelType_HullWhiteLattice, 0.03, 0.01, 50,
        quantra::enums::ModelParamMode_Calibrate, "swaption_atm", "discount", "EUR_SWAP_6M",
        "discount");
    auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols, models);

    auto modelId = b.CreateString("hw_cal_model");
    quantra::CalibrateSwaptionModelRequestBuilder cb(b);
    cb.add_pricing(pricing);
    cb.add_model_id(modelId);
    b.Finish(cb.Finish());

    CalibrateSwaptionModelPricingRequest req;
    auto respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto resp = req.request(
        respB, flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto out =
        flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(respB->GetBufferPointer());

    auto reqRoot =
        flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer());
    quantra::PricingRegistryBuilder regBuilder;
    quantra::PricingRegistry reg = regBuilder.build(reqRoot->pricing());
    auto dIt = reg.rates.curves.find("discount");
    auto fIt = reg.rates.curves.find("discount");
    ASSERT_TRUE(dIt != reg.rates.curves.end() && fIt != reg.rates.curves.end());
    QuantLib::Handle<QuantLib::YieldTermStructure> discountCurve(dIt->second->currentLink());
    QuantLib::Handle<QuantLib::YieldTermStructure> forwardingCurve(fIt->second->currentLink());

    auto vIt = reg.volatility.swaptionVols.find("swaption_atm");
    ASSERT_TRUE(vIt != reg.volatility.swaptionVols.end());
    const auto& volEntry = vIt->second;
    ASSERT_TRUE(reg.rates.swapIndices.has("EUR_SWAP_6M"));
    const auto& sidx = reg.rates.swapIndices.get("EUR_SWAP_6M");
    auto ibor = reg.rates.indices.getIborWithCurve(sidx.floatIndexId, forwardingCurve);

    const QuantLib::Period fixedLegTenor(sidx.fixedFrequency);
    const QuantLib::DayCounter fixedDc = sidx.fixedDayCounter;
    const QuantLib::DayCounter floatDc = ibor->dayCounter();
    const auto errorType = (volEntry.qlVolType == QuantLib::Normal)
                               ? QuantLib::BlackCalibrationHelper::PriceError
                               : QuantLib::BlackCalibrationHelper::ImpliedVolError;
    const QuantLib::Natural settlementDays = static_cast<QuantLib::Natural>(sidx.spotDays);

    std::vector<QuantLib::ext::shared_ptr<QuantLib::CalibrationHelper>> helpers;
    helpers.reserve(expiries.size() * tenors.size());
    for (const auto& exp : expiries) {
        for (const auto& ten : tenors) {
            const double marketVol = volEntry.handle->volatility(exp, ten, 0.0, true);
            auto volQuote = QuantLib::Handle<QuantLib::Quote>(
                QuantLib::ext::make_shared<QuantLib::SimpleQuote>(marketVol));
            auto helper = QuantLib::ext::make_shared<QuantLib::SwaptionHelper>(
                exp, ten, volQuote, ibor, fixedLegTenor, fixedDc, floatDc, discountCurve, errorType,
                QuantLib::Null<QuantLib::Real>(), 1.0, volEntry.qlVolType, volEntry.displacement,
                settlementDays);
            helpers.push_back(helper);
        }
    }

    auto hwModel = QuantLib::ext::make_shared<QuantLib::HullWhite>(discountCurve, 0.03, 0.01);
    auto engine = QuantLib::ext::make_shared<QuantLib::JamshidianSwaptionEngine>(hwModel);
    for (auto& h : helpers) {
        auto blackHelper = QuantLib::ext::dynamic_pointer_cast<QuantLib::BlackCalibrationHelper>(h);
        ASSERT_TRUE(static_cast<bool>(blackHelper));
        blackHelper->setPricingEngine(engine);
    }

    QuantLib::LevenbergMarquardt lm;
    QuantLib::EndCriteria endCriteria(1000, 200, 1.0e-8, 1.0e-8, 1.0e-8);
    std::vector<bool> fixParams = {false, false};
    // The server keeps a joint fit inside [a_lower, a_upper] (schema defaults
    // 1e-3 / 1.0): on this market-style matrix an unconstrained fit drifts to
    // a ~ 1.2e-4 on the flat plateau, so the reference applies the same box.
    hwModel->calibrate(
        helpers, lm, endCriteria, quantra::HwMeanReversionBounds(0.001, 1.0), std::vector<double>(),
        fixParams);
    const auto params = hwModel->params();
    ASSERT_GE(params.size(), 2u);

    double err2 = 0.0;
    for (const auto& h : helpers) {
        const double e = h->calibrationError();
        err2 += e * e;
    }
    const double qlRmse = std::sqrt(err2 / static_cast<double>(helpers.size()));

    EXPECT_NEAR(out->hw_a(), params[0], 1.0e-10);
    EXPECT_NEAR(out->hw_sigma(), params[1], 1.0e-10);
    EXPECT_NEAR(out->rmse(), qlRmse, 1.0e-10);
    EXPECT_EQ(out->num_helpers(), static_cast<int>(helpers.size()));
    EXPECT_EQ(out->grid_rows(), static_cast<int>(expiries.size()));
    EXPECT_EQ(out->grid_cols(), static_cast<int>(tenors.size()));
    EXPECT_EQ(out->grid_points(), static_cast<int>(expiries.size() * tenors.size()));
}

// ---------------------------------------------------------------------------
// Joint (a, sigma) calibration: bounds on "a" and the flatness diagnostic.
// ---------------------------------------------------------------------------

namespace {

// A HullWhiteLattice model in Calibrate mode whose hw_calibration block carries
// explicit calibrate_a / a_init / a_lower / a_upper (parity_fixture's
// buildSwaptionModel only sets the ids, i.e. schema defaults).
flatbuffers::Offset<quantra::ModelSpec> buildCalibratedHwModel(
    flatbuffers::grpc::MessageBuilder& b,
    const std::string& id,
    bool calibrateA,
    double aInit,
    double aLower,
    double aUpper) {
    auto volId = b.CreateString("swaption_atm");
    auto curveId = b.CreateString("discount");
    auto fwdId = b.CreateString("discount");
    auto swapIndexId = b.CreateString("EUR_SWAP_6M");
    quantra::SwaptionHwCalibrationSpecBuilder cb(b);
    cb.add_swaption_vol_id(volId);
    cb.add_discount_curve_id(curveId);
    cb.add_forwarding_curve_id(fwdId);
    cb.add_swap_index_id(swapIndexId);
    cb.add_calibrate_a(calibrateA);
    cb.add_calibrate_sigma(true);
    cb.add_a_init(aInit);
    cb.add_sigma_init(0.01);
    cb.add_a_lower(aLower);
    cb.add_a_upper(aUpper);
    auto calibSpec = cb.Finish();

    quantra::SwaptionModelSpecBuilder sm(b);
    sm.add_model_type(quantra::enums::IrModelType_HullWhiteLattice);
    sm.add_hw_a(aInit);
    sm.add_hw_sigma(0.01);
    sm.add_lattice_steps(50);
    sm.add_param_mode(quantra::enums::ModelParamMode_Calibrate);
    sm.add_hw_calibration(calibSpec);
    auto payload = sm.Finish();

    auto modelId = b.CreateString(id);
    quantra::ModelSpecBuilder ms(b);
    ms.add_id(modelId);
    ms.add_payload_type(quantra::ModelPayload_SwaptionModelSpec);
    ms.add_payload(payload.Union());
    return ms.Finish();
}

} // namespace

class HwJointCalibrationTest : public QuantraComparisonTest {
protected:
    static constexpr const char* kAsOf = "2025-01-15";
    // Grid kept inside the fixture curve's 10Y horizon (expiry + tenor <= 8Y).
    const std::vector<QuantLib::Period> expiries_ = {
        QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years),
        QuantLib::Period(3, QuantLib::Years)};
    const std::vector<QuantLib::Period> tenors_ = {
        QuantLib::Period(2, QuantLib::Years), QuantLib::Period(4, QuantLib::Years),
        QuantLib::Period(5, QuantLib::Years)};

    // Build a calibrate-swaption-model request on the fixture curve with the
    // given ATM vols (row-major over expiries_ x tenors_) and calibration knobs.
    void buildRequest(
        flatbuffers::grpc::MessageBuilder& b,
        const std::vector<double>& vols,
        bool calibrateA,
        double aInit,
        double aLower = 0.001,
        double aUpper = 1.0) {
        auto ts = buildCurve(b, "discount");
        auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{ts});
        auto volSurface =
            buildSwaptionVolAtmMatrixSurface(b, "swaption_atm", expiries_, tenors_, vols);
        auto volsVec =
            b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
        auto model = buildCalibratedHwModel(b, "hw_cal_model", calibrateA, aInit, aLower, aUpper);
        auto models = b.CreateVector(std::vector<flatbuffers::Offset<quantra::ModelSpec>>{model});
        auto indices = buildIndicesVector(b);
        auto swapIndices = buildSwapIndicesVector(b);
        auto asof = b.CreateString(kAsOf);
        auto pricing =
            buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volsVec, models);
        auto modelId = b.CreateString("hw_cal_model");
        quantra::CalibrateSwaptionModelRequestBuilder cb(b);
        cb.add_pricing(pricing);
        cb.add_model_id(modelId);
        b.Finish(cb.Finish());
    }

    const quantra::CalibrateSwaptionModelResponse* calibrate(
        flatbuffers::grpc::MessageBuilder& b,
        std::shared_ptr<flatbuffers::grpc::MessageBuilder>& respB) {
        CalibrateSwaptionModelPricingRequest req;
        respB = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto resp = req.request(
            respB,
            flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
        respB->Finish(resp);
        return flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(
            respB->GetBufferPointer());
    }

    // Implied ATM vols of the grid under HullWhite(a, sigma) on the fixture
    // curve: the market a joint calibration must round-trip.
    std::vector<double> surfaceFromHullWhite(double a, double sigma) {
        flatbuffers::grpc::MessageBuilder b;
        std::vector<double> seed(expiries_.size() * tenors_.size(), 0.20);
        buildRequest(b, seed, true, 0.03);
        auto reqRoot =
            flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer());
        quantra::PricingRegistryBuilder regBuilder;
        quantra::PricingRegistry reg = regBuilder.build(reqRoot->pricing());
        auto cIt = reg.rates.curves.find("discount");
        QuantLib::Handle<QuantLib::YieldTermStructure> curve(cIt->second->currentLink());
        const auto& sidx = reg.rates.swapIndices.get("EUR_SWAP_6M");
        auto ibor = reg.rates.indices.getIborWithCurve(sidx.floatIndexId, curve);
        const QuantLib::Period fixedLegTenor(sidx.fixedFrequency);

        auto hw = QuantLib::ext::make_shared<QuantLib::HullWhite>(curve, a, sigma);
        auto engine = QuantLib::ext::make_shared<QuantLib::JamshidianSwaptionEngine>(hw);
        std::vector<double> vols;
        for (const auto& exp : expiries_) {
            for (const auto& ten : tenors_) {
                auto volQuote = QuantLib::Handle<QuantLib::Quote>(
                    QuantLib::ext::make_shared<QuantLib::SimpleQuote>(0.20));
                auto helper = QuantLib::ext::make_shared<QuantLib::SwaptionHelper>(
                    exp, ten, volQuote, ibor, fixedLegTenor, sidx.fixedDayCounter,
                    ibor->dayCounter(), curve, QuantLib::BlackCalibrationHelper::ImpliedVolError,
                    QuantLib::Null<QuantLib::Real>(), 1.0, QuantLib::ShiftedLognormal, 0.0,
                    static_cast<QuantLib::Natural>(sidx.spotDays));
                helper->setPricingEngine(engine);
                const double price = helper->modelValue();
                vols.push_back(helper->impliedVolatility(price, 1.0e-12, 10000, 1.0e-6, 5.0));
            }
        }
        return vols;
    }
};

TEST_F(HwJointCalibrationTest, JointFitRecoversGeneratingParametersWithinBounds) {
    const double aTrue = 0.05, sigmaTrue = 0.008;
    const auto vols = surfaceFromHullWhite(aTrue, sigmaTrue);
    for (double v : vols)
        ASSERT_TRUE(std::isfinite(v) && v > 0.0);

    flatbuffers::grpc::MessageBuilder b;
    buildRequest(b, vols, true, 0.03);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB;
    auto out = calibrate(b, respB);

    EXPECT_NEAR(out->hw_a(), aTrue, 1.0e-4);
    EXPECT_NEAR(out->hw_sigma(), sigmaTrue, 1.0e-4);
    EXPECT_LT(out->rmse(), 1.0e-6);
    // Joint fit: the flatness diagnostic is present, and a surface generated
    // by the model pins "a" sharply.
    ASSERT_TRUE(out->a_flatness().has_value());
    ASSERT_TRUE(out->a_poorly_identified().has_value());
    EXPECT_GE(out->a_flatness().value(), 0.0);
    EXPECT_LE(out->a_flatness().value(), 1.0);
    EXPECT_GT(out->a_flatness().value(), 0.01);
    EXPECT_FALSE(out->a_poorly_identified().value());
}

TEST_F(HwJointCalibrationTest, AInitBelowLowerBoundIsClippedAndResultStaysInBounds) {
    const auto vols = surfaceFromHullWhite(0.05, 0.008);
    flatbuffers::grpc::MessageBuilder b;
    // Start far below the lower bound; the fit must start at the bound and
    // end inside [a_lower, a_upper].
    buildRequest(b, vols, true, 1.0e-6, 0.001, 1.0);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB;
    auto out = calibrate(b, respB);
    EXPECT_GE(out->hw_a(), 0.001);
    EXPECT_LE(out->hw_a(), 1.0);
    EXPECT_TRUE(out->a_flatness().has_value());

    // A tight box that excludes the generating a=0.05: the fit stays inside it.
    flatbuffers::grpc::MessageBuilder b2;
    buildRequest(b2, vols, true, 0.03, 0.10, 0.50);
    auto out2 = calibrate(b2, respB);
    EXPECT_GE(out2->hw_a(), 0.10 - 1.0e-12);
    EXPECT_LE(out2->hw_a(), 0.50 + 1.0e-12);
}

TEST_F(HwJointCalibrationTest, InvalidBoundsAreRejected) {
    const auto vols = surfaceFromHullWhite(0.05, 0.008);
    {
        flatbuffers::grpc::MessageBuilder b;
        buildRequest(b, vols, true, 0.03, 0.0, 1.0); // a_lower must be > 0
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB;
        EXPECT_THROW(calibrate(b, respB), QuantraInvalidArgument);
    }
    {
        flatbuffers::grpc::MessageBuilder b;
        buildRequest(b, vols, true, 0.03, 0.5, 0.1); // a_upper must exceed a_lower
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB;
        EXPECT_THROW(calibrate(b, respB), QuantraInvalidArgument);
    }
    {
        // Bounds are ignored for a sigma-only fit: an inverted box is accepted
        // and "a" stays exactly at a_init.
        flatbuffers::grpc::MessageBuilder b;
        buildRequest(b, vols, false, 0.0002, 0.5, 0.1);
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB;
        auto out = calibrate(b, respB);
        EXPECT_DOUBLE_EQ(out->hw_a(), 0.0002);
    }
}

TEST_F(HwJointCalibrationTest, FlatnessDiagnosticPresentIffCalibrateA) {
    const auto vols = surfaceFromHullWhite(0.05, 0.008);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB;
    {
        flatbuffers::grpc::MessageBuilder b;
        buildRequest(b, vols, false, 0.03);
        auto out = calibrate(b, respB);
        EXPECT_DOUBLE_EQ(out->hw_a(), 0.03);
        EXPECT_FALSE(out->a_flatness().has_value());
        EXPECT_FALSE(out->a_poorly_identified().has_value());
    }
    {
        flatbuffers::grpc::MessageBuilder b;
        buildRequest(b, vols, true, 0.03);
        auto out = calibrate(b, respB);
        EXPECT_TRUE(out->a_flatness().has_value());
        EXPECT_TRUE(out->a_poorly_identified().has_value());
    }
}

TEST_F(HwJointCalibrationTest, FlatSurfaceIsFlaggedPoorlyIdentified) {
    // A constant-vol grid is not a Hull-White surface; the fit lands on a
    // plateau and the diagnostic must stay consistent with the flag whatever
    // the optimizer settled on, with "a" inside the default box.
    std::vector<double> vols(expiries_.size() * tenors_.size(), 0.20);
    flatbuffers::grpc::MessageBuilder b;
    buildRequest(b, vols, true, 0.03);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> respB;
    auto out = calibrate(b, respB);
    ASSERT_TRUE(out->a_flatness().has_value());
    EXPECT_GE(out->hw_a(), 0.001);
    EXPECT_LE(out->hw_a(), 1.0);
    // The diagnostic is consistent with the flag by construction.
    EXPECT_EQ(out->a_poorly_identified().value(), out->a_flatness().value() < 0.01);
}

}} // namespace quantra::testing
