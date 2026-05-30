// Sample Vol Surfaces parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "parity_fixture.h"

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrParamsReturnsFinitePositiveVols) {
    SabrSyntheticGrid g;

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionSabrParamsSurface(
        b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.02, 0.03, 0.04, 0.05});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();

    auto disc = b.CreateString("discount");
    auto fwd = b.CreateString("discount");
    auto volId = b.CreateString("sabr_vol");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    qsb.add_discounting_curve_id(disc);
    qsb.add_forwarding_curve_id(fwd);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    if (r->error() != nullptr) {
        FAIL() << "sample failed: "
               << (r->error()->error_message() ? r->error()->error_message()->c_str() : "(no msg)");
    }
    // 2 expiries x 2 tenors x 4 strikes
    ASSERT_EQ(r->vols()->size(), 16u);
    for (flatbuffers::uoffset_t i = 0; i < r->vols()->size(); ++i) {
        double v = r->vols()->Get(i);
        EXPECT_TRUE(std::isfinite(v)) << "non-finite vol at index " << i;
        EXPECT_GT(v, 0.0) << "non-positive vol at index " << i;
    }
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrCalibrateReturnsFiniteVols) {
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
        b, "sabr_calibrate_sample", g.expiries, g.tenors, spreads, syntheticVols);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.02, 0.03, 0.04});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();

    auto disc = b.CreateString("discount");
    auto fwd = b.CreateString("discount");
    auto volId = b.CreateString("sabr_calibrate_sample");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    qsb.add_discounting_curve_id(disc);
    qsb.add_forwarding_curve_id(fwd);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    if (r->error() != nullptr) {
        FAIL() << "sample failed: "
               << (r->error()->error_message() ? r->error()->error_message()->c_str() : "(no msg)");
    }
    ASSERT_EQ(r->vols()->size(), 12u);
    for (flatbuffers::uoffset_t i = 0; i < r->vols()->size(); ++i) {
        double v = r->vols()->Get(i);
        EXPECT_TRUE(std::isfinite(v)) << "non-finite vol at index " << i;
        EXPECT_GT(v, 0.0) << "non-positive vol at index " << i;
    }
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrCalibrateDiagnosticsPopulated) {
    SabrCalibrateCache::instance().clear();
    SabrSyntheticGrid g;
    const std::vector<double> spreads{-0.02, -0.01, 0.0, 0.01, 0.02};
    std::vector<double> forwards(4, 0.03);
    std::vector<QuantLib::Real> tte(4, 1.0);
    auto syntheticVols = sabrSyntheticMarketVols(g, forwards, tte, spreads);

    auto buildSampleRequest = [&](bool includeDiagnostics) {
        auto b = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto curve = buildLongCurve(*b, "discount");
        auto curves = b->CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        auto vs = buildSwaptionSabrCalibrateSurface(
            *b, "sabr_calibrate_diag", g.expiries, g.tenors, spreads, syntheticVols);
        auto vols = b->CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
        auto indices = buildIndicesVector(*b);
        auto swapIndices = buildSwapIndicesVector(*b);
        auto asof = b->CreateString("2025-01-15");
        auto pricing = buildPricing(*b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

        quantra::PeriodBuilder e1(*b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
        auto e1Off = e1.Finish();
        quantra::PeriodBuilder e2(*b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
        auto e2Off = e2.Finish();
        auto expVec = b->CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
        quantra::TenorGridBuilder expGridB(*b); expGridB.add_tenors(expVec);
        auto expGrid = expGridB.Finish();
        quantra::DateGridSpecBuilder expSpecB(*b);
        expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
        expSpecB.add_grid(expGrid.Union());
        auto expSpec = expSpecB.Finish();

        quantra::PeriodBuilder t5(*b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
        auto t5Off = t5.Finish();
        quantra::PeriodBuilder t10(*b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
        auto t10Off = t10.Finish();
        auto tenVec = b->CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
        quantra::TenorGridBuilder tenGridB(*b); tenGridB.add_tenors(tenVec);
        auto tenGrid = tenGridB.Finish();
        quantra::DateGridSpecBuilder tenSpecB(*b);
        tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
        tenSpecB.add_grid(tenGrid.Union());
        auto tenSpec = tenSpecB.Finish();

        auto strikeVals = b->CreateVector(std::vector<double>{0.02, 0.03, 0.04});
        quantra::StrikeGridBuilder sgb(*b);
        sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
        sgb.add_strikes(strikeVals);
        auto strikeGrid = sgb.Finish();

        auto disc = b->CreateString("discount");
        auto fwd = b->CreateString("discount");
        auto volId = b->CreateString("sabr_calibrate_diag");
        auto swapIdx = b->CreateString("EUR_SWAP_6M");
        quantra::VolQuerySpecBuilder qsb(*b);
        qsb.add_vol_id(volId);
        qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
        qsb.add_expiry_grid(expSpec);
        qsb.add_tenor_grid(tenSpec);
        qsb.add_strike_grid(strikeGrid);
        qsb.add_swap_index_id(swapIdx);
        qsb.add_discounting_curve_id(disc);
        qsb.add_forwarding_curve_id(fwd);
        auto query = qsb.Finish();
        auto queries = b->CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
        quantra::SampleVolSurfacesRequestBuilder rb(*b);
        rb.add_pricing(pricing);
        rb.add_queries(queries);
        rb.add_include_diagnostics(includeDiagnostics);
        b->Finish(rb.Finish());
        return b;
    };

    auto runSample = [&](bool includeDiagnostics) {
        auto b = buildSampleRequest(includeDiagnostics);
        SampleVolSurfacesRequestHandler handler;
        auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto out = handler.request(
            outBuilder,
            flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b->GetBufferPointer()));
        outBuilder->Finish(out);
        return std::make_pair(
            outBuilder,
            flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer()));
    };

    // include_diagnostics=false: response carries no diagnostics vector.
    auto noDiag = runSample(false);
    auto* respNo = noDiag.second;
    EXPECT_TRUE(respNo->diagnostics() == nullptr || respNo->diagnostics()->size() == 0u)
        << "diagnostics array unexpectedly present when include_diagnostics=false";

    // include_diagnostics=true: one populated SwaptionVolDiagnostics with full
    // surface block + calibration sub-block + converged=true.
    auto withDiag = runSample(true);
    auto* respYes = withDiag.second;
    ASSERT_NE(respYes->diagnostics(), nullptr);
    ASSERT_EQ(respYes->diagnostics()->size(), 1u);
    auto* d = respYes->diagnostics()->Get(0);
    ASSERT_NE(d, nullptr);
    EXPECT_EQ(d->kind(), quantra::enums::SwaptionVolKind_SabrCalibrate);
    ASSERT_NE(d->alpha_per_node(), nullptr);
    EXPECT_EQ(d->alpha_per_node()->size(), 4u);
    ASSERT_NE(d->beta_per_node(), nullptr);
    ASSERT_NE(d->rho_per_node(), nullptr);
    ASSERT_NE(d->nu_per_node(), nullptr);
    ASSERT_NE(d->forward_per_node(), nullptr);
    ASSERT_NE(d->atm_vol_per_node(), nullptr);
    auto* calib = d->calibration();
    ASSERT_NE(calib, nullptr) << "calibration sub-block missing on SabrCalibrate";
    EXPECT_TRUE(calib->converged());
    EXPECT_TRUE(std::isfinite(calib->overall_rmse()));
    ASSERT_NE(calib->per_node_rmse(), nullptr);
    EXPECT_EQ(calib->per_node_rmse()->size(), 4u);
    SabrCalibrateCache::instance().clear();
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrParamsDiagnosticsHasNoCalibrationBlock) {
    SabrSyntheticGrid g;

    flatbuffers::grpc::MessageBuilder b;
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto vs = buildSwaptionSabrParamsSurface(
        b, "sabr_params_diag", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto vols = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.03});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();

    auto disc = b.CreateString("discount");
    auto fwd = b.CreateString("discount");
    auto volId = b.CreateString("sabr_params_diag");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    qsb.add_discounting_curve_id(disc);
    qsb.add_forwarding_curve_id(fwd);
    auto query = qsb.Finish();
    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    rb.add_include_diagnostics(true);
    b.Finish(rb.Finish());

    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(
        outBuilder,
        flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer()));
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(
        outBuilder->GetBufferPointer());
    ASSERT_NE(resp->diagnostics(), nullptr);
    ASSERT_EQ(resp->diagnostics()->size(), 1u);
    auto* d = resp->diagnostics()->Get(0);
    EXPECT_EQ(d->kind(), quantra::enums::SwaptionVolKind_SabrParams);
    // SABR-params surface: surface block populated, calibration sub-block null.
    ASSERT_NE(d->alpha_per_node(), nullptr);
    EXPECT_EQ(d->alpha_per_node()->size(), 4u);
    ASSERT_NE(d->forward_per_node(), nullptr);
    ASSERT_NE(d->atm_vol_per_node(), nullptr);
    EXPECT_EQ(d->calibration(), nullptr)
        << "params-path diagnostics must not carry a calibration sub-block";
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionConstantCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSurface(b, "swp_const", 0.20);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b); tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid); tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikes = b.CreateVector(std::vector<double>{0.01, 0.02});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikes);
    auto strikeGrid = sgb.Finish();

    auto volId = b.CreateString("swp_const");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 8u);
    EXPECT_EQ(r->atm_levels()->size(), 0u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSpreadFromAtmReturnsAtmGrid) {
    flatbuffers::grpc::MessageBuilder b;
    std::vector<QuantLib::Period> expiries = {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    std::vector<QuantLib::Period> tenors = {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    std::vector<double> strikes = {-0.0025, 0.0, 0.0025};
    std::vector<double> vols(expiries.size() * tenors.size() * strikes.size(), 0.01);
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swp_spread", expiries, tenors, strikes, vols,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M");
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(1); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(2); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b); tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid); tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{-0.001, 0.0, 0.001});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_SpreadFromATM); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto disc = b.CreateString("discount");
    auto fwd = b.CreateString("discount");
    auto volId = b.CreateString("swp_spread");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_swap_index_id(swapIdx);
    qsb.add_discounting_curve_id(disc);
    qsb.add_forwarding_curve_id(fwd);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 12u);
    EXPECT_EQ(r->atm_levels()->size(), 4u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_OptionletCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildOptionletVolSurface(b, "opt_const", 0.25, quantra::enums::VolatilityType_Normal);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.01, 0.02, 0.03});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("opt_const");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Optionlet);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 6u);
    EXPECT_EQ(r->tenors()->size(), 0u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(b, "EQVOL_CONST", 0.25, quantra::enums::VolSurfaceShape_Constant, "2026-02-27");
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2026-02-27");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(6); e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b); e3.add_n(1); e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("EQVOL_CONST");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->n_expiries(), 3);
    EXPECT_EQ(r->n_tenors(), 0);
    EXPECT_EQ(r->n_strikes(), 3);
    ASSERT_EQ(r->vols()->size(), 9u);
    for (flatbuffers::uoffset_t i = 0; i < r->vols()->size(); ++i) {
        EXPECT_NEAR(r->vols()->Get(i), 0.25, 1.0e-12);
    }
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackTermStructureCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolTermStructure(
        b, "eq_black_term",
        {QuantLib::Period(1, QuantLib::Months), QuantLib::Period(6, QuantLib::Months), QuantLib::Period(1, QuantLib::Years)},
        {0.20, 0.22, 0.25});
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(6); e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b); e3.add_n(1); e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_term");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    ASSERT_EQ(r->vols()->size(), 9u);
    EXPECT_NEAR(r->vols()->Get(0), 0.20, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(3), 0.22, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(6), 0.25, 1.0e-12);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackSurfaceCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurfaceGrid(
        b, "eq_black_surface",
        {QuantLib::Period(6, QuantLib::Months), QuantLib::Period(1, QuantLib::Years)},
        {80.0, 100.0, 120.0},
        {0.30, 0.25, 0.22, 0.32, 0.27, 0.24});
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(6); e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(1); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_surface");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    ASSERT_EQ(r->vols()->size(), 6u);
    EXPECT_NEAR(r->vols()->Get(0), 0.30, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(1), 0.25, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(2), 0.22, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(3), 0.32, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(4), 0.27, 1.0e-12);
    EXPECT_NEAR(r->vols()->Get(5), 0.24, 1.0e-12);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackSurfaceFromPricesMatchesDirectQuantLib) {
    const QuantLib::Date refDate(15, QuantLib::January, 2025);
    const QuantLib::Calendar cal = QuantLib::TARGET();
    const QuantLib::BusinessDayConvention bdc = QuantLib::ModifiedFollowing;
    const QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    const double spot = 100.0;
    const std::vector<QuantLib::Date> pillarDates = {
        QuantLib::Date(15, QuantLib::July, 2025),
        QuantLib::Date(15, QuantLib::January, 2026)
    };
    const std::vector<double> strikes = {80.0, 100.0, 120.0};
    const std::vector<double> inputVolsFlat = {
        0.30, 0.25, 0.22,  // 2025-07-15
        0.32, 0.27, 0.24   // 2026-01-15
    };

    // Build a direct QuantLib surface to generate option prices at the input nodes.
    QuantLib::Matrix inputVolMatrix(static_cast<int>(strikes.size()), static_cast<int>(pillarDates.size()));
    for (size_t i = 0; i < pillarDates.size(); ++i) {
        for (size_t j = 0; j < strikes.size(); ++j) {
            inputVolMatrix[static_cast<int>(j)][static_cast<int>(i)] =
                inputVolsFlat[i * strikes.size() + j];
        }
    }
    auto sourceSurface = std::make_shared<QuantLib::BlackVarianceSurface>(
        refDate, cal, pillarDates, strikes, inputVolMatrix, dc);
    sourceSurface->setInterpolation<QuantLib::Bilinear>();

    auto spotHandle = QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot));
    auto sourceProcess = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        spotHandle,
        dividendHandle_,
        discountHandle_,
        QuantLib::Handle<QuantLib::BlackVolTermStructure>(sourceSurface));
    std::vector<double> priceMatrixFlat;
    priceMatrixFlat.reserve(pillarDates.size() * strikes.size());
    for (const auto& expiry : pillarDates) {
        auto ex = std::make_shared<QuantLib::EuropeanExercise>(expiry);
        for (double k : strikes) {
            auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, k);
            QuantLib::VanillaOption opt(payoff, ex);
            opt.setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(sourceProcess));
            priceMatrixFlat.push_back(opt.NPV());
        }
    }

    // Reconstruct implied vol nodes directly in QuantLib from the generated prices.
    auto seedVol = std::make_shared<QuantLib::BlackConstantVol>(refDate, cal, 0.20, dc);
    auto inversionProcess = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        spotHandle,
        dividendHandle_,
        discountHandle_,
        QuantLib::Handle<QuantLib::BlackVolTermStructure>(seedVol));
    QuantLib::Matrix impliedNodeVols(static_cast<int>(strikes.size()), static_cast<int>(pillarDates.size()));
    for (size_t i = 0; i < pillarDates.size(); ++i) {
        auto ex = std::make_shared<QuantLib::EuropeanExercise>(pillarDates[i]);
        for (size_t j = 0; j < strikes.size(); ++j) {
            auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, strikes[j]);
            QuantLib::VanillaOption opt(payoff, ex);
            const double mktPrice = priceMatrixFlat[i * strikes.size() + j];
            impliedNodeVols[static_cast<int>(j)][static_cast<int>(i)] =
                opt.impliedVolatility(mktPrice, inversionProcess, 1.0e-8, 500, 1.0e-8, 10.0);
        }
    }
    auto expectedSurface = std::make_shared<QuantLib::BlackVarianceSurface>(
        refDate, cal, pillarDates, strikes, impliedNodeVols, dc);
    expectedSurface->setInterpolation<QuantLib::Bilinear>();

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurfaceFromPrices(
        b,
        "eq_black_prices",
        {"2025-07-15", "2026-01-15"},
        strikes,
        priceMatrixFlat,
        "EQ_SPOT",
        "discount",
        "div");
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curveDiscount = buildCurve(b, "discount", flatRate_);
    auto curveDividend = buildCurve(b, "div", dividendFlatRate_);
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{
        curveDiscount, curveDividend});
    auto indices = buildIndicesVector(b);

    auto quoteId = b.CreateString("EQ_SPOT");
    quantra::QuoteSpecBuilder qb(b);
    qb.add_id(quoteId);
    qb.add_kind(quantra::QuoteKind_Price);
    qb.add_value(spot);
    qb.add_quote_type(quantra::QuoteType_Curve);
    auto quote = qb.Finish();
    auto quotes = b.CreateVector(std::vector<flatbuffers::Offset<quantra::QuoteSpec>>{quote});

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, quotes, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(6); e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(9); e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b); e3.add_n(1); e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    expGridB.add_calendar(quantra::enums::Calendar_TARGET);
    expGridB.add_business_day_convention(quantra::enums::BusinessDayConvention_ModifiedFollowing);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(strikes);
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_prices");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    ASSERT_EQ(r->n_expiries(), 3);
    ASSERT_EQ(r->n_strikes(), 3);
    ASSERT_EQ(r->vols()->size(), 9u);

    const std::vector<QuantLib::Date> queryDates = {
        cal.advance(refDate, QuantLib::Period(6, QuantLib::Months), bdc),
        cal.advance(refDate, QuantLib::Period(9, QuantLib::Months), bdc),
        cal.advance(refDate, QuantLib::Period(1, QuantLib::Years), bdc)
    };
    const std::vector<double> queryStrikes = strikes;
    for (size_t i = 0; i < queryDates.size(); ++i) {
        for (size_t j = 0; j < queryStrikes.size(); ++j) {
            const double expected = expectedSurface->blackVol(queryDates[i], queryStrikes[j]);
            const double actual = r->vols()->Get(static_cast<flatbuffers::uoffset_t>(i * queryStrikes.size() + j));
            EXPECT_NEAR(actual, expected, 2.0e-4);
        }
    }

    // Also verify exact node recovery at original pillar grid from endpoint output.
    // Query order is [6M, 9M, 1Y], so original pillars map to row 0 and row 2.
    for (size_t j = 0; j < strikes.size(); ++j) {
        const double expected6M = impliedNodeVols[static_cast<int>(j)][0];
        const double actual6M = r->vols()->Get(static_cast<flatbuffers::uoffset_t>(j));
        EXPECT_NEAR(actual6M, expected6M, 2.0e-4);

        const double expected1Y = impliedNodeVols[static_cast<int>(j)][1];
        const double actual1Y = r->vols()->Get(static_cast<flatbuffers::uoffset_t>(2 * strikes.size() + j));
        EXPECT_NEAR(actual1Y, expected1Y, 2.0e-4);
    }
}

TEST_F(QuantraComparisonTest, BlackVolSurface_NodeOrientationMatchesInput) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurfaceGrid(
        b, "eq_black_orientation",
        {QuantLib::Period(1, QuantLib::Months), QuantLib::Period(2, QuantLib::Months)},
        {90.0, 100.0},
        {0.10, 0.11, 0.20, 0.21},
        "2026-01-02");
    b.Finish(volSurface);

    auto spec = flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer());
    auto entry = parseBlackVol(spec, nullptr);
    ASSERT_FALSE(entry.handle.empty());

    const QuantLib::Date ref(2, QuantLib::January, 2026);
    const QuantLib::Calendar cal = QuantLib::TARGET();
    const QuantLib::BusinessDayConvention bdc = QuantLib::ModifiedFollowing;
    const QuantLib::Date d1M = cal.advance(ref, QuantLib::Period(1, QuantLib::Months), bdc);
    const QuantLib::Date d2M = cal.advance(ref, QuantLib::Period(2, QuantLib::Months), bdc);

    EXPECT_NEAR(entry.handle->blackVol(d1M, 90.0), 0.10, 1.0e-12);
    EXPECT_NEAR(entry.handle->blackVol(d1M, 100.0), 0.11, 1.0e-12);
    EXPECT_NEAR(entry.handle->blackVol(d2M, 90.0), 0.20, 1.0e-12);
    EXPECT_NEAR(entry.handle->blackVol(d2M, 100.0), 0.21, 1.0e-12);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackRejectsSmileSlice) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(b, "eq_black_const", 0.20);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1.Finish()});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{100.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_const");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_output_mode(quantra::VolOutputMode_SmileSlice);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackRejectsSpreadFromAtmAxis) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(b, "eq_black_const", 0.20);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1.Finish()});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{-5.0, 0.0, 5.0});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_SpreadFromATM); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    auto volId = b.CreateString("eq_black_const");

    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_EquityBlack);
    qsb.add_expiry_grid(expSpec);
    qsb.add_strike_grid(strikeGrid);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_MaxPointsGuard) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSurface(b, "swp_const", 0.20);
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b); e1.add_n(1); e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b); e2.add_n(2); e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b); t10.add_n(10); t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b); tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid); tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.01, 0.02, 0.03});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();

    quantra::QueryOptionsBuilder qob(b);
    qob.add_max_points(2);
    auto opts = qob.Finish();
    auto volId = b.CreateString("swp_const");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_options(opts);
    qsb.add_swap_index_id(swapIdx);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_NoExtrapolationGuard) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swp_abs",
        {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)},
        {QuantLib::Period(5, QuantLib::Years)},
        {0.01, 0.02},
        std::vector<double>(2 * 1 * 2, 0.01),
        quantra::enums::SwaptionStrikeKind_Absolute, "EUR_SWAP_6M");
    auto volSurfaces = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e5(b); e5.add_n(5); e5.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e5.Finish()});
    quantra::TenorGridBuilder expGridB(b); expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b); expSpecB.add_grid_type(quantra::DateGrid_TenorGrid); expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b); t5.add_n(5); t5.add_unit(quantra::enums::TimeUnit_Years);
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5.Finish()});
    quantra::TenorGridBuilder tenGridB(b); tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b); tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid); tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.05});
    quantra::StrikeGridBuilder sgb(b); sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike); sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    quantra::QueryOptionsBuilder qob(b); qob.add_allow_extrapolation(false);
    auto opts = qob.Finish();
    auto volId = b.CreateString("swp_abs");
    auto swapIdx = b.CreateString("EUR_SWAP_6M");
    quantra::VolQuerySpecBuilder qsb(b);
    qsb.add_vol_id(volId);
    qsb.add_surface_type(quantra::VolSurfaceType_Swaption);
    qsb.add_expiry_grid(expSpec);
    qsb.add_tenor_grid(tenSpec);
    qsb.add_strike_grid(strikeGrid);
    qsb.add_options(opts);
    qsb.add_swap_index_id(swapIdx);
    auto query = qsb.Finish();

    auto queries = b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
    quantra::SampleVolSurfacesRequestBuilder rb(b);
    rb.add_pricing(pricing);
    rb.add_queries(queries);
    b.Finish(rb.Finish());

    auto req = flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer());
    SampleVolSurfacesRequestHandler handler;
    auto outBuilder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
    auto out = handler.request(outBuilder, req);
    outBuilder->Finish(out);
    auto resp = flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

}} // namespace quantra::testing
