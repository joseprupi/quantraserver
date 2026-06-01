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

    std::vector<QuantLib::Period> expiries = { QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years) };
    std::vector<QuantLib::Period> tenors = { QuantLib::Period(5, QuantLib::Years), QuantLib::Period(7, QuantLib::Years) };
    auto volSurface = buildSwaptionVolAtmMatrixSurface(
        b, "swaption_atm", expiries, tenors, {0.20, 0.21, 0.22, 0.23});
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(
        b, "hw_cal_model", quantra::enums::IrModelType_HullWhiteLattice, 0.03, 0.01, 50,
        quantra::enums::ModelParamMode_Calibrate, "swaption_atm", "discount", "EUR_SWAP_6M", "discount");
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
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto out = flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(respB->GetBufferPointer());

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
        QuantLib::Period(1, QuantLib::Years),
        QuantLib::Period(2, QuantLib::Years)
    };
    std::vector<QuantLib::Period> tenors = {
        QuantLib::Period(5, QuantLib::Years),
        QuantLib::Period(7, QuantLib::Years)
    };
    std::vector<double> volsFlat = {0.20, 0.21, 0.22, 0.23};
    auto volSurface = buildSwaptionVolAtmMatrixSurface(b, "swaption_atm", expiries, tenors, volsFlat);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});

    auto model = buildSwaptionModel(
        b, "hw_cal_model", quantra::enums::IrModelType_HullWhiteLattice, 0.03, 0.01, 50,
        quantra::enums::ModelParamMode_Calibrate, "swaption_atm", "discount", "EUR_SWAP_6M", "discount");
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
    auto resp = req.request(respB, flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer()));
    respB->Finish(resp);
    auto out = flatbuffers::GetRoot<quantra::CalibrateSwaptionModelResponse>(respB->GetBufferPointer());

    auto reqRoot = flatbuffers::GetRoot<quantra::CalibrateSwaptionModelRequest>(b.GetBufferPointer());
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
    const auto errorType =
        (volEntry.qlVolType == QuantLib::Normal)
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
                exp,
                ten,
                volQuote,
                ibor,
                fixedLegTenor,
                fixedDc,
                floatDc,
                discountCurve,
                errorType,
                QuantLib::Null<QuantLib::Real>(),
                1.0,
                volEntry.qlVolType,
                volEntry.displacement,
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
    hwModel->calibrate(helpers, lm, endCriteria, QuantLib::NoConstraint(), std::vector<double>(), fixParams);
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

}} // namespace quantra::testing
