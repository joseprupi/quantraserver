// Calibrate Swaption Vol parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, CalibrateSwaptionVolEndpoint_HappyPath) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto vs = buildSwaptionSabrCalibrateSurface(
        b, "sabr_calibrate_endpoint", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    auto volIdOff = b.CreateString("sabr_calibrate_endpoint");
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
    ASSERT_NE(resp->vol_id(), nullptr);
    EXPECT_EQ(resp->vol_id()->str(), "sabr_calibrate_endpoint");
    auto* d = resp->diagnostics();
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind(), quantra::enums::SwaptionVolKind_SabrCalibrate);
    ASSERT_NE(d->alpha_per_node(), nullptr);
    EXPECT_EQ(d->alpha_per_node()->size(), 4u);
    auto* calib = d->calibration();
    ASSERT_NE(calib, nullptr);
    EXPECT_TRUE(calib->converged());
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, CalibrateSwaptionVolEndpoint_RejectsBadInputs) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    auto buildBaseContext = [&](
            std::function<void(flatbuffers::grpc::MessageBuilder&,
                               std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>&)> volSurfaceFn,
            const std::string& volId,
            const std::string& discId,
            const std::string& fwdId) {
        auto b = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto curve = buildLongCurve(*b, "discount");
        auto curves = b->CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>> volSurfaces;
        volSurfaceFn(*b, volSurfaces);
        auto vols = b->CreateVector(volSurfaces);
        auto indices = buildIndicesVector(*b);
        auto swapIndices = buildSwapIndicesVector(*b);
        auto asof = b->CreateString("2025-01-15");
        auto pricing = buildPricing(*b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

        auto volIdOff = b->CreateString(volId);
        auto discIdOff = b->CreateString(discId);
        auto fwdIdOff = b->CreateString(fwdId);
        quantra::CalibrateSwaptionVolRequestBuilder rb(*b);
        rb.add_pricing(pricing);
        rb.add_vol_id(volIdOff);
        rb.add_discounting_curve_id(discIdOff);
        rb.add_forwarding_curve_id(fwdIdOff);
        b->Finish(rb.Finish());
        return b;
    };

    auto callHandler = [](flatbuffers::grpc::MessageBuilder& msgB) {
        CalibrateSwaptionVolPricingRequest handler;
        auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        return handler.request(
            outBuilder,
            flatbuffers::GetRoot<quantra::CalibrateSwaptionVolRequest>(msgB.GetBufferPointer()));
    };

    // (a) vol_id absent from the registry -> NOT_FOUND.
    {
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionSabrCalibrateSurface(
                    b, "sabr_calibrate_present", g.expiries, g.tenors, spreads, syntheticVols));
            },
            /*volId=*/"sabr_calibrate_missing",
            /*discId=*/"discount",
            /*fwdId=*/"discount");
        EXPECT_THROW(callHandler(*msgB), QuantraNotFound);
    }

    // (b) vol_id resolves to AtmMatrix2D (non-SabrCalibrate) -> INVALID_ARGUMENT.
    {
        std::vector<double> atmVols{0.20, 0.21, 0.22, 0.23};
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionVolAtmMatrixSurface(
                    b, "atm_matrix", g.expiries, g.tenors, atmVols));
            },
            /*volId=*/"atm_matrix",
            /*discId=*/"discount",
            /*fwdId=*/"discount");
        EXPECT_THROW(callHandler(*msgB), QuantraInvalidArgument);
    }

    // (c) vol_id resolves to SabrParams (also non-SabrCalibrate) -> INVALID_ARGUMENT.
    {
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionSabrParamsSurface(
                    b, "sabr_params_only", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu));
            },
            /*volId=*/"sabr_params_only",
            /*discId=*/"discount",
            /*fwdId=*/"discount");
        EXPECT_THROW(callHandler(*msgB), QuantraInvalidArgument);
    }

    // (d) discounting_curve_id missing from registry -> INVALID_ARGUMENT.
    {
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionSabrCalibrateSurface(
                    b, "sabr_calibrate_present", g.expiries, g.tenors, spreads, syntheticVols));
            },
            /*volId=*/"sabr_calibrate_present",
            /*discId=*/"missing_curve",
            /*fwdId=*/"discount");
        EXPECT_THROW(callHandler(*msgB), QuantraInvalidArgument);
    }

    // (e) forwarding_curve_id missing from registry -> INVALID_ARGUMENT.
    {
        auto msgB = buildBaseContext(
            [&](flatbuffers::grpc::MessageBuilder& b,
                std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>& v) {
                v.push_back(buildSwaptionSabrCalibrateSurface(
                    b, "sabr_calibrate_present", g.expiries, g.tenors, spreads, syntheticVols));
            },
            /*volId=*/"sabr_calibrate_present",
            /*discId=*/"discount",
            /*fwdId=*/"missing_fwd_curve");
        EXPECT_THROW(callHandler(*msgB), QuantraInvalidArgument);
    }
    SabrCalibrateCache::instance().clear();
}

}} // namespace quantra::testing
