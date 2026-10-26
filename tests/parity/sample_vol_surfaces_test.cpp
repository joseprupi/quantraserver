// Sample Vol Surfaces parity tests.
//
// Relocated verbatim from tests/test_quantra_vs_quantlib.cpp (refactor 6a).
// Shares QuantraComparisonTest from parity_fixture.h; built into the single
// test_quantra_vs_quantlib gtest binary.
#include "basket_model.h"
#include "parity_fixture.h"

#include <ql/experimental/volatility/svismilesection.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>

#include <functional>

namespace quantra { namespace testing {

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrParamsReturnsFinitePositiveVols) {
    SabrSyntheticGrid g;

    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionSabrParamsSurface(
        b, "sabr_vol", g.expiries, g.tenors, g.alpha, g.beta, g.rho, g.nu);
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildLongCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(2);
    e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b);
    t5.add_n(5);
    t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b);
    t10.add_n(10);
    t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b);
    tenGridB.add_tenors(tenVec);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
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

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(2);
    e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b);
    t5.add_n(5);
    t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b);
    t10.add_n(10);
    t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b);
    tenGridB.add_tenors(tenVec);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
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
        auto curves =
            b->CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
        auto vs = buildSwaptionSabrCalibrateSurface(
            *b, "sabr_calibrate_diag", g.expiries, g.tenors, spreads, syntheticVols);
        auto vols = b->CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{vs});
        auto indices = buildIndicesVector(*b);
        auto swapIndices = buildSwapIndicesVector(*b);
        auto asof = b->CreateString("2025-01-15");
        auto pricing = buildPricing(*b, asof, 0, 0, indices, swapIndices, curves, 0, 0, vols);

        quantra::PeriodBuilder e1(*b);
        e1.add_n(1);
        e1.add_unit(quantra::enums::TimeUnit_Years);
        auto e1Off = e1.Finish();
        quantra::PeriodBuilder e2(*b);
        e2.add_n(2);
        e2.add_unit(quantra::enums::TimeUnit_Years);
        auto e2Off = e2.Finish();
        auto expVec =
            b->CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
        quantra::TenorGridBuilder expGridB(*b);
        expGridB.add_tenors(expVec);
        auto expGrid = expGridB.Finish();
        quantra::DateGridSpecBuilder expSpecB(*b);
        expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
        expSpecB.add_grid(expGrid.Union());
        auto expSpec = expSpecB.Finish();

        quantra::PeriodBuilder t5(*b);
        t5.add_n(5);
        t5.add_unit(quantra::enums::TimeUnit_Years);
        auto t5Off = t5.Finish();
        quantra::PeriodBuilder t10(*b);
        t10.add_n(10);
        t10.add_unit(quantra::enums::TimeUnit_Years);
        auto t10Off = t10.Finish();
        auto tenVec =
            b->CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
        quantra::TenorGridBuilder tenGridB(*b);
        tenGridB.add_tenors(tenVec);
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
        auto queries =
            b->CreateVector(std::vector<flatbuffers::Offset<quantra::VolQuerySpec>>{query});
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
            outBuilder, flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(
                            outBuilder->GetBufferPointer()));
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

TEST_F(
    QuantraComparisonTest, SampleVolSurfaces_SwaptionSabrParamsDiagnosticsHasNoCalibrationBlock) {
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

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(2);
    e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b);
    t5.add_n(5);
    t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b);
    t10.add_n(10);
    t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b);
    tenGridB.add_tenors(tenVec);
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
        outBuilder, flatbuffers::GetRoot<quantra::SampleVolSurfacesRequest>(b.GetBufferPointer()));
    outBuilder->Finish(out);
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
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
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(2);
    e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b);
    t5.add_n(5);
    t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b);
    t10.add_n(10);
    t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b);
    tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikes = b.CreateVector(std::vector<double>{0.01, 0.02});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikes);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 8u);
    EXPECT_EQ(r->atm_levels()->size(), 0u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_SwaptionSpreadFromAtmReturnsAtmGrid) {
    flatbuffers::grpc::MessageBuilder b;
    std::vector<QuantLib::Period> expiries = {
        QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    std::vector<QuantLib::Period> tenors = {
        QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
    std::vector<double> strikes = {-0.0025, 0.0, 0.0025};
    std::vector<double> vols(expiries.size() * tenors.size() * strikes.size(), 0.01);
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swp_spread", expiries, tenors, strikes, vols,
        quantra::enums::SwaptionStrikeKind_SpreadFromATM, "EUR_SWAP_6M");
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(2);
    e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b);
    t5.add_n(1);
    t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b);
    t10.add_n(2);
    t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b);
    tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{-0.001, 0.0, 0.001});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_SpreadFromATM);
    sgb.add_strikes(strikeVals);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 12u);
    EXPECT_EQ(r->atm_levels()->size(), 4u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_OptionletCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface =
        buildOptionletVolSurface(b, "opt_const", 0.25, quantra::enums::VolatilityType_Normal);
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(2);
    e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.01, 0.02, 0.03});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    EXPECT_EQ(r->vols()->size(), 6u);
    EXPECT_EQ(r->tenors()->size(), 0u);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackCube) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(
        b, "EQVOL_CONST", 0.25, quantra::enums::VolSurfaceShape_Constant, "2026-02-27");
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2026-02-27");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(6);
    e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b);
    e3.add_n(1);
    e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

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
        {QuantLib::Period(1, QuantLib::Months), QuantLib::Period(6, QuantLib::Months),
         QuantLib::Period(1, QuantLib::Years)},
        {0.20, 0.22, 0.25});
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(6);
    e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b);
    e3.add_n(1);
    e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

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
        {80.0, 100.0, 120.0}, {0.30, 0.25, 0.22, 0.32, 0.27, 0.24});
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(6);
    e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(1);
    e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{80.0, 100.0, 120.0});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

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
        QuantLib::Date(15, QuantLib::July, 2025), QuantLib::Date(15, QuantLib::January, 2026)};
    const std::vector<double> strikes = {80.0, 100.0, 120.0};
    const std::vector<double> inputVolsFlat = {
        0.30, 0.25, 0.22, // 2025-07-15
        0.32, 0.27, 0.24  // 2026-01-15
    };

    // Build a direct QuantLib surface to generate option prices at the input nodes.
    QuantLib::Matrix inputVolMatrix(
        static_cast<int>(strikes.size()), static_cast<int>(pillarDates.size()));
    for (size_t i = 0; i < pillarDates.size(); ++i) {
        for (size_t j = 0; j < strikes.size(); ++j) {
            inputVolMatrix[static_cast<int>(j)][static_cast<int>(i)] =
                inputVolsFlat[i * strikes.size() + j];
        }
    }
    auto sourceSurface = std::make_shared<QuantLib::BlackVarianceSurface>(
        refDate, cal, pillarDates, strikes, inputVolMatrix, dc);
    sourceSurface->setInterpolation<QuantLib::Bilinear>();

    auto spotHandle =
        QuantLib::Handle<QuantLib::Quote>(std::make_shared<QuantLib::SimpleQuote>(spot));
    auto sourceProcess = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        spotHandle, dividendHandle_, discountHandle_,
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
        spotHandle, dividendHandle_, discountHandle_,
        QuantLib::Handle<QuantLib::BlackVolTermStructure>(seedVol));
    QuantLib::Matrix impliedNodeVols(
        static_cast<int>(strikes.size()), static_cast<int>(pillarDates.size()));
    for (size_t i = 0; i < pillarDates.size(); ++i) {
        auto ex = std::make_shared<QuantLib::EuropeanExercise>(pillarDates[i]);
        for (size_t j = 0; j < strikes.size(); ++j) {
            auto payoff =
                std::make_shared<QuantLib::PlainVanillaPayoff>(QuantLib::Option::Call, strikes[j]);
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
        b, "eq_black_prices", {"2025-07-15", "2026-01-15"}, strikes, priceMatrixFlat, "EQ_SPOT",
        "discount", "div");
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curveDiscount = buildCurve(b, "discount", flatRate_);
    auto curveDividend = buildCurve(b, "div", dividendFlatRate_);
    auto curves = b.CreateVector(
        std::vector<flatbuffers::Offset<quantra::TermStructure>>{curveDiscount, curveDividend});
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

    quantra::PeriodBuilder e1(b);
    e1.add_n(6);
    e1.add_unit(quantra::enums::TimeUnit_Months);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(9);
    e2.add_unit(quantra::enums::TimeUnit_Months);
    auto e2Off = e2.Finish();
    quantra::PeriodBuilder e3(b);
    e3.add_n(1);
    e3.add_unit(quantra::enums::TimeUnit_Years);
    auto e3Off = e3.Finish();
    auto expVec =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off, e3Off});
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    auto r = resp->results()->Get(0);
    ASSERT_EQ(r->error(), nullptr);
    ASSERT_EQ(r->n_expiries(), 3);
    ASSERT_EQ(r->n_strikes(), 3);
    ASSERT_EQ(r->vols()->size(), 9u);

    const std::vector<QuantLib::Date> queryDates = {
        cal.advance(refDate, QuantLib::Period(6, QuantLib::Months), bdc),
        cal.advance(refDate, QuantLib::Period(9, QuantLib::Months), bdc),
        cal.advance(refDate, QuantLib::Period(1, QuantLib::Years), bdc)};
    const std::vector<double> queryStrikes = strikes;
    for (size_t i = 0; i < queryDates.size(); ++i) {
        for (size_t j = 0; j < queryStrikes.size(); ++j) {
            const double expected = expectedSurface->blackVol(queryDates[i], queryStrikes[j]);
            const double actual =
                r->vols()->Get(static_cast<flatbuffers::uoffset_t>(i * queryStrikes.size() + j));
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
        const double actual1Y =
            r->vols()->Get(static_cast<flatbuffers::uoffset_t>(2 * strikes.size() + j));
        EXPECT_NEAR(actual1Y, expected1Y, 2.0e-4);
    }
}

TEST_F(QuantraComparisonTest, BlackVolSurface_NodeOrientationMatchesInput) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurfaceGrid(
        b, "eq_black_orientation",
        {QuantLib::Period(1, QuantLib::Months), QuantLib::Period(2, QuantLib::Months)},
        {90.0, 100.0}, {0.10, 0.11, 0.20, 0.21}, "2026-01-02");
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

// Strike extrapolation of a parsed strike x expiry surface is constant: a
// strike beyond the grid reads the edge strike's vol on both sides (also
// after the vega shift rebuild), and beyond the last expiry the implied vol
// is flat (QuantLib scales the last total variance linearly in time). The
// 80 / 100 / 120 grid with vols 0.50 / 0.35 / 0.20 is a skew whose linear
// continuation of the variance would turn negative above ~133.
TEST_F(QuantraComparisonTest, BlackVolSurface_StrikeExtrapolationIsConstant) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurfaceGrid(
        b, "eq_black_edge",
        {QuantLib::Period(1, QuantLib::Months), QuantLib::Period(2, QuantLib::Months)},
        {80.0, 100.0, 120.0}, {0.50, 0.35, 0.20, 0.50, 0.35, 0.20}, "2026-01-02");
    b.Finish(volSurface);
    auto spec = flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer());
    auto entry = parseBlackVol(spec, nullptr);
    ASSERT_FALSE(entry.handle.empty());

    const QuantLib::Date ref(2, QuantLib::January, 2026);
    const QuantLib::Calendar cal = QuantLib::TARGET();
    const QuantLib::BusinessDayConvention bdc = QuantLib::ModifiedFollowing;
    const QuantLib::Date d1M = cal.advance(ref, QuantLib::Period(1, QuantLib::Months), bdc);
    const QuantLib::Date d2M = cal.advance(ref, QuantLib::Period(2, QuantLib::Months), bdc);
    const QuantLib::Date d6M = cal.advance(ref, QuantLib::Period(6, QuantLib::Months), bdc);
    const auto& h = entry.handle;
    EXPECT_NEAR(h->minStrike(), 80.0, 0.0);
    EXPECT_NEAR(h->maxStrike(), 120.0, 0.0);
    EXPECT_NEAR(h->blackVol(d1M, 50.0, true), 0.50, 1.0e-12);
    EXPECT_NEAR(h->blackVol(d1M, 1.0, true), 0.50, 1.0e-12);
    EXPECT_NEAR(h->blackVol(d1M, 160.0, true), 0.20, 1.0e-12);
    EXPECT_NEAR(h->blackVol(d2M, 1000.0, true), 0.20, 1.0e-12);
    EXPECT_EQ(h->blackVariance(d1M, 160.0, true), h->blackVariance(d1M, 120.0, true));
    EXPECT_NEAR(
        h->blackVol(d1M, 110.0, true), std::sqrt(0.5 * (0.35 * 0.35 + 0.20 * 0.20)),
        1.0e-12); // bilinear in variance inside the grid, as before
    // Beyond the last expiry: the same implied vol as at the last expiry.
    EXPECT_NEAR(h->blackVol(d6M, 100.0, true), h->blackVol(d2M, 100.0, true), 1.0e-12);
    EXPECT_NEAR(h->blackVol(d6M, 50.0, true), 0.50, 1.0e-12);
    // The vega-shift rebuild keeps the policy.
    const auto shifted = shiftedBlackVol(entry, 0.01);
    EXPECT_NEAR(shifted->blackVol(d1M, 50.0, true), 0.51, 1.0e-12);
    EXPECT_NEAR(shifted->blackVol(d1M, 160.0, true), 0.21, 1.0e-12);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackRejectsSmileSlice) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(b, "eq_black_const", 0.20);
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1.Finish()});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{100.0});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_EquityBlackRejectsSpreadFromAtmAxis) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildBlackVolSurface(b, "eq_black_const", 0.20);
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);

    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, 0, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1.Finish()});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{-5.0, 0.0, 5.0});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_SpreadFromATM);
    sgb.add_strikes(strikeVals);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());

    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_MaxPointsGuard) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSurface(b, "swp_const", 0.20);
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");

    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e1(b);
    e1.add_n(1);
    e1.add_unit(quantra::enums::TimeUnit_Years);
    auto e1Off = e1.Finish();
    quantra::PeriodBuilder e2(b);
    e2.add_n(2);
    e2.add_unit(quantra::enums::TimeUnit_Years);
    auto e2Off = e2.Finish();
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e1Off, e2Off});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b);
    t5.add_n(5);
    t5.add_unit(quantra::enums::TimeUnit_Years);
    auto t5Off = t5.Finish();
    quantra::PeriodBuilder t10(b);
    t10.add_n(10);
    t10.add_unit(quantra::enums::TimeUnit_Years);
    auto t10Off = t10.Finish();
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5Off, t10Off});
    quantra::TenorGridBuilder tenGridB(b);
    tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.01, 0.02, 0.03});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

TEST_F(QuantraComparisonTest, SampleVolSurfaces_NoExtrapolationGuard) {
    flatbuffers::grpc::MessageBuilder b;
    auto volSurface = buildSwaptionVolSmileCubeSurface(
        b, "swp_abs", {QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)},
        {QuantLib::Period(5, QuantLib::Years)}, {0.01, 0.02}, std::vector<double>(2 * 1 * 2, 0.01),
        quantra::enums::SwaptionStrikeKind_Absolute, "EUR_SWAP_6M");
    auto volSurfaces =
        b.CreateVector(std::vector<flatbuffers::Offset<quantra::VolSurfaceSpec>>{volSurface});
    auto curve = buildCurve(b, "discount");
    auto curves = b.CreateVector(std::vector<flatbuffers::Offset<quantra::TermStructure>>{curve});
    auto indices = buildIndicesVector(b);
    auto swapIndices = buildSwapIndicesVector(b);
    auto asof = b.CreateString("2025-01-15");
    auto pricing = buildPricing(b, asof, 0, 0, indices, swapIndices, curves, 0, 0, volSurfaces);

    quantra::PeriodBuilder e5(b);
    e5.add_n(5);
    e5.add_unit(quantra::enums::TimeUnit_Years);
    auto expVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{e5.Finish()});
    quantra::TenorGridBuilder expGridB(b);
    expGridB.add_tenors(expVec);
    auto expGrid = expGridB.Finish();
    quantra::DateGridSpecBuilder expSpecB(b);
    expSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    expSpecB.add_grid(expGrid.Union());
    auto expSpec = expSpecB.Finish();

    quantra::PeriodBuilder t5(b);
    t5.add_n(5);
    t5.add_unit(quantra::enums::TimeUnit_Years);
    auto tenVec = b.CreateVector(std::vector<flatbuffers::Offset<quantra::Period>>{t5.Finish()});
    quantra::TenorGridBuilder tenGridB(b);
    tenGridB.add_tenors(tenVec);
    auto tenGrid = tenGridB.Finish();
    quantra::DateGridSpecBuilder tenSpecB(b);
    tenSpecB.add_grid_type(quantra::DateGrid_TenorGrid);
    tenSpecB.add_grid(tenGrid.Union());
    auto tenSpec = tenSpecB.Finish();

    auto strikeVals = b.CreateVector(std::vector<double>{0.05});
    quantra::StrikeGridBuilder sgb(b);
    sgb.add_axis(quantra::VolStrikeAxis_AbsoluteStrike);
    sgb.add_strikes(strikeVals);
    auto strikeGrid = sgb.Finish();
    quantra::QueryOptionsBuilder qob(b);
    qob.add_allow_extrapolation(false);
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
    auto resp =
        flatbuffers::GetRoot<quantra::SampleVolSurfacesResponse>(outBuilder->GetBufferPointer());
    ASSERT_EQ(resp->results()->size(), 1u);
    ASSERT_NE(resp->results()->Get(0)->error(), nullptr);
}

// ---------------------------------------------------------------------------
// CubicStrikeLinearTime grids and SviVolSpec payloads
// (src/market/smile_slice_vol_surface.h through vol_surface_parsers).
// ---------------------------------------------------------------------------

namespace {

// Raw SVI slice used to write the test grids and the SVI payloads: a
// 16-day and a 1-year smile with the SSVI shape of the synthetic sheets.
struct TestSlice {
    std::string expiryIso;
    double T, F, a, b, sigma, rho, m;
    double vol(double K) const {
        return std::sqrt(quantra::sviRawTotalVariance(a, b, sigma, rho, m, std::log(K / F)) / T);
    }
};

std::vector<TestSlice> testSlices() {
    return {
        {"2026-10-16", 16.0 / 365.0, 100.5, 0.000591, 0.024078, 0.034390, -0.7, 0.033709},
        {"2027-09-30", 365.0 / 365.0, 103.0, 0.011246, 0.105000, 0.150000, -0.7, 0.147000}};
}

flatbuffers::Offset<quantra::BlackVolBaseSpec> testVolBase(
    flatbuffers::grpc::MessageBuilder& b, bool withShape) {
    auto ref = b.CreateString("2026-09-30");
    quantra::BlackVolBaseSpecBuilder bb(b);
    bb.add_reference_date(ref);
    bb.add_calendar(quantra::enums::Calendar_NullCalendar);
    bb.add_business_day_convention(quantra::enums::BusinessDayConvention_Unadjusted);
    bb.add_day_counter(quantra::enums::DayCounter_Actual365Fixed);
    if (withShape) bb.add_shape(quantra::enums::VolSurfaceShape_SmileCube3D);
    return bb.Finish();
}

flatbuffers::Offset<quantra::VolSurfaceSpec> testGridSpec(
    flatbuffers::grpc::MessageBuilder& b,
    const std::vector<double>& strikes,
    quantra::enums::SurfaceInterpolator2D interp) {
    const auto slices = testSlices();
    std::vector<flatbuffers::Offset<quantra::Period>> periods;
    std::vector<double> vols;
    for (const auto& sl : slices) {
        quantra::PeriodBuilder pb(b);
        pb.add_n(static_cast<int>(std::lround(sl.T * 365.0)));
        pb.add_unit(quantra::enums::TimeUnit_Days);
        periods.push_back(pb.Finish());
        for (double K : strikes)
            vols.push_back(sl.vol(K));
    }
    auto base = testVolBase(b, true);
    auto periodsOff = b.CreateVector(periods);
    auto strikesOff = b.CreateVector(strikes);
    auto valuesOff = b.CreateVector(vols);
    quantra::QuoteMatrix2DBuilder mb(b);
    mb.add_n_rows(static_cast<int>(slices.size()));
    mb.add_n_cols(static_cast<int>(strikes.size()));
    mb.add_values(valuesOff);
    auto matrix = mb.Finish();
    quantra::BlackVolSpecBuilder vb(b);
    vb.add_base(base);
    vb.add_expiries(periodsOff);
    vb.add_strikes(strikesOff);
    vb.add_surface_vols(matrix);
    vb.add_allow_extrapolation(true);
    vb.add_surface_interpolator(interp);
    auto payload = vb.Finish();
    auto id = b.CreateString("grid");
    quantra::VolSurfaceSpecBuilder sb(b);
    sb.add_id(id);
    sb.add_payload_type(quantra::VolPayload_BlackVolSpec);
    sb.add_payload(payload.Union());
    return sb.Finish();
}

flatbuffers::Offset<quantra::VolSurfaceSpec> testSviSpec(
    flatbuffers::grpc::MessageBuilder& b,
    const std::function<void(std::vector<TestSlice>&)>& edit) {
    auto slices = testSlices();
    edit(slices);
    std::vector<flatbuffers::Offset<quantra::SviSlice>> offs;
    for (const auto& sl : slices) {
        auto exp = b.CreateString(sl.expiryIso);
        quantra::SviSliceBuilder slb(b);
        slb.add_expiry(exp);
        slb.add_forward(sl.F);
        slb.add_a(sl.a);
        slb.add_b(sl.b);
        slb.add_sigma(sl.sigma);
        slb.add_rho(sl.rho);
        slb.add_m(sl.m);
        offs.push_back(slb.Finish());
    }
    auto base = testVolBase(b, false);
    auto slicesOff = b.CreateVector(offs);
    quantra::SviVolSpecBuilder svb(b);
    svb.add_base(base);
    svb.add_slices(slicesOff);
    svb.add_allow_extrapolation(true);
    auto payload = svb.Finish();
    auto id = b.CreateString("svi");
    quantra::VolSurfaceSpecBuilder sb(b);
    sb.add_id(id);
    sb.add_payload_type(quantra::VolPayload_SviVolSpec);
    sb.add_payload(payload.Union());
    return sb.Finish();
}

} // namespace

TEST_F(QuantraComparisonTest, BlackVolSurface_CubicStrikeLinearTimeKeepsTheSmileBetweenNodes) {
    std::vector<double> strikes;
    for (int j = 0; j <= 20; ++j)
        strikes.push_back(50.0 + 5.0 * j); // 50 .. 150 every 5
    const auto slices = testSlices();
    const QuantLib::Date ref(30, QuantLib::September, 2026);
    const QuantLib::Date d16 = ref + 16, d1y = ref + 365, dMid = ref + 100, dFar = ref + 800;

    auto parse = [&](quantra::enums::SurfaceInterpolator2D interp) {
        flatbuffers::grpc::MessageBuilder b;
        b.Finish(testGridSpec(b, strikes, interp));
        auto spec = flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer());
        return parseBlackVol(spec, nullptr);
    };
    const auto cubic = parse(quantra::enums::SurfaceInterpolator2D_CubicStrikeLinearTime);
    const auto bilinear = parse(quantra::enums::SurfaceInterpolator2D_Bilinear);
    ASSERT_EQ(cubic.surfaceInterpolation, BlackSurfaceInterpolation::CubicStrikeLinearTime);
    EXPECT_FALSE(mc::IsExactStepCapable(cubic.handle));
    EXPECT_NEAR(cubic.handle->minStrike(), 50.0, 0.0);
    EXPECT_NEAR(cubic.handle->maxStrike(), 150.0, 0.0);

    // Every node is reproduced; the mid-nodes sit on the generating smile
    // (natural cubic spline of total variance in log-strike) where the
    // bilinear reading is off by up to half a vol point on the short wing.
    double worstNode = 0.0, worstCubic = 0.0, worstBilinear = 0.0;
    for (std::size_t i = 0; i < slices.size(); ++i) {
        const QuantLib::Date d = i == 0 ? d16 : d1y;
        for (std::size_t j = 0; j < strikes.size(); ++j) {
            worstNode = std::max(
                worstNode,
                std::fabs(cubic.handle->blackVol(d, strikes[j], true) - slices[i].vol(strikes[j])));
            if (j + 1 < strikes.size()) {
                const double K = 0.5 * (strikes[j] + strikes[j + 1]);
                worstCubic = std::max(
                    worstCubic, std::fabs(cubic.handle->blackVol(d, K, true) - slices[i].vol(K)));
                worstBilinear = std::max(
                    worstBilinear,
                    std::fabs(bilinear.handle->blackVol(d, K, true) - slices[i].vol(K)));
            }
        }
    }
    EXPECT_LT(worstNode, 1.0e-13);
    EXPECT_LT(worstCubic, 2.0e-3);
    EXPECT_GT(worstBilinear, 4.0e-3);
    std::cout << "  cubic grid: node error " << worstNode << ", mid-node error cubic " << worstCubic
              << " vs bilinear " << worstBilinear << std::endl;

    // Constant beyond the strike edges, linear in total variance between
    // the expiries at fixed strike, flat implied vol beyond the last.
    EXPECT_EQ(
        cubic.handle->blackVariance(d16, 10.0, true), cubic.handle->blackVariance(d16, 50.0, true));
    EXPECT_EQ(
        cubic.handle->blackVariance(d1y, 900.0, true),
        cubic.handle->blackVariance(d1y, 150.0, true));
    const double K = 92.5, t16 = 16.0 / 365.0, t1y = 1.0, tMid = 100.0 / 365.0;
    const double w16 = cubic.handle->blackVariance(d16, K, true);
    const double w1y = cubic.handle->blackVariance(d1y, K, true);
    EXPECT_NEAR(
        cubic.handle->blackVariance(dMid, K, true), w16 + (w1y - w16) * (tMid - t16) / (t1y - t16),
        1.0e-15);
    EXPECT_NEAR(cubic.handle->blackVariance(ref + 8, K, true), w16 * (8.0 / 365.0) / t16, 1.0e-15);
    EXPECT_NEAR(
        cubic.handle->blackVol(dFar, K, true), cubic.handle->blackVol(d1y, K, true), 1.0e-14);

    // The vega-shift rebuild keeps the interpolation: +1 vol point at the nodes.
    const auto shifted = shiftedBlackVol(cubic, 0.01);
    EXPECT_NEAR(
        shifted->blackVol(d1y, 100.0, true), cubic.handle->blackVol(d1y, 100.0, true) + 0.01,
        1.0e-13);
    EXPECT_NEAR(
        shifted->blackVol(d16, 55.0, true), cubic.handle->blackVol(d16, 55.0, true) + 0.01,
        1.0e-13);

    // Dupire's local vol over it is finite and positive inside the grid
    // (the bilinear reading has no curvature inside a cell: the local vol
    // there is that of a flat-in-strike variance, wrong but finite too).
    QuantLib::Handle<QuantLib::YieldTermStructure> flat(
        std::make_shared<QuantLib::FlatForward>(ref, 0.03, QuantLib::Actual365Fixed()));
    QuantLib::Handle<QuantLib::YieldTermStructure> zero(
        std::make_shared<QuantLib::FlatForward>(ref, 0.0, QuantLib::Actual365Fixed()));
    QuantLib::Handle<QuantLib::Quote> spot(std::make_shared<QuantLib::SimpleQuote>(100.0));
    QuantLib::LocalVolSurface lv(cubic.handle, flat, zero, spot);
    for (double level : {60.0, 80.0, 92.5, 100.0, 107.5, 120.0, 140.0}) {
        const double v = lv.localVol(0.5, level, true);
        EXPECT_TRUE(std::isfinite(v) && v > 0.0) << level;
    }

    // One strike cannot be read cubic.
    {
        flatbuffers::grpc::MessageBuilder b;
        b.Finish(
            testGridSpec(b, {100.0}, quantra::enums::SurfaceInterpolator2D_CubicStrikeLinearTime));
        auto spec = flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer());
        EXPECT_THROW(parseBlackVol(spec, nullptr), QuantraInvalidArgument);
    }
}

TEST_F(QuantraComparisonTest, SviVolSpec_EvaluatesTheSlicesAndInterpolatesTotalVariance) {
    const auto slices = testSlices();
    const QuantLib::Date ref(30, QuantLib::September, 2026);
    const QuantLib::Date d16 = ref + 16, d1y = ref + 365, dMid = ref + 100, dFar = ref + 800;
    flatbuffers::grpc::MessageBuilder b;
    b.Finish(testSviSpec(b, [](std::vector<TestSlice>&) {}));
    auto spec = flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer());
    const auto entry = parseSviVol(spec);
    ASSERT_FALSE(entry.handle.empty());
    EXPECT_EQ(entry.shapeFb, quantra::enums::VolSurfaceShape_SviSlices);
    EXPECT_FALSE(mc::IsExactStepCapable(entry.handle));
    EXPECT_EQ(entry.handle->minStrike(), 0.0);
    EXPECT_EQ(entry.handle->maxStrike(), QL_MAX_REAL);
    EXPECT_EQ(entry.handle->maxDate(), d1y);

    // At a slice's expiry the vol is the slice's SVI vol (QuantLib's
    // SviSmileSection agrees); between slices linear in total variance;
    // before the first w1 t / T1; beyond the last flat implied vol.
    for (double K : {40.0, 70.0, 90.0, 100.0, 110.0, 130.0, 250.0}) {
        EXPECT_NEAR(entry.handle->blackVol(d16, K, true), slices[0].vol(K), 1.0e-14) << K;
        EXPECT_NEAR(entry.handle->blackVol(d1y, K, true), slices[1].vol(K), 1.0e-14) << K;
        QuantLib::SviSmileSection section(
            slices[1].T, slices[1].F,
            {slices[1].a, slices[1].b, slices[1].sigma, slices[1].rho, slices[1].m});
        EXPECT_NEAR(entry.handle->blackVol(d1y, K, true), section.volatility(K), 1.0e-14) << K;
        const double w16 = entry.handle->blackVariance(d16, K, true);
        const double w1y = entry.handle->blackVariance(d1y, K, true);
        const double t16 = 16.0 / 365.0, tMid = 100.0 / 365.0;
        EXPECT_NEAR(
            entry.handle->blackVariance(dMid, K, true),
            w16 + (w1y - w16) * (tMid - t16) / (1.0 - t16), 1.0e-15);
        EXPECT_NEAR(
            entry.handle->blackVariance(ref + 4, K, true), w16 * (4.0 / 365.0) / t16, 1.0e-16);
        EXPECT_NEAR(entry.handle->blackVol(dFar, K, true), slices[1].vol(K), 1.0e-14);
    }
    // The shift adds the bump to every vol, at every read.
    const auto shifted = shiftedBlackVol(entry, 0.02);
    EXPECT_NEAR(shifted->blackVol(d1y, 85.0, true), slices[1].vol(85.0) + 0.02, 1.0e-14);
    EXPECT_NEAR(
        shifted->blackVol(dMid, 115.0, true), entry.handle->blackVol(dMid, 115.0, true) + 0.02,
        1.0e-14);
    EXPECT_THROW(shiftedBlackVol(entry, -0.5)->blackVol(d1y, 100.0, true), QuantLib::Error);

    // Dupire's local vol over the slices: finite, positive, no edge.
    QuantLib::Handle<QuantLib::YieldTermStructure> flat(
        std::make_shared<QuantLib::FlatForward>(ref, 0.03, QuantLib::Actual365Fixed()));
    QuantLib::Handle<QuantLib::YieldTermStructure> zero(
        std::make_shared<QuantLib::FlatForward>(ref, 0.0, QuantLib::Actual365Fixed()));
    QuantLib::Handle<QuantLib::Quote> spot(std::make_shared<QuantLib::SimpleQuote>(100.0));
    QuantLib::LocalVolSurface lv(entry.handle, flat, zero, spot);
    for (double level : {30.0, 60.0, 100.0, 140.0, 300.0}) {
        const double v = lv.localVol(0.5, level, true);
        EXPECT_TRUE(std::isfinite(v) && v > 0.0) << level;
    }

    // Rejected slices, each naming the field.
    auto rejects = [&](const std::function<void(std::vector<TestSlice>&)>& edit,
                       const std::string& needle) {
        flatbuffers::grpc::MessageBuilder bb;
        bb.Finish(testSviSpec(bb, edit));
        auto bad = flatbuffers::GetRoot<quantra::VolSurfaceSpec>(bb.GetBufferPointer());
        try {
            parseSviVol(bad);
            FAIL() << needle;
        } catch (const QuantraInvalidArgument& e) {
            EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
        }
    };
    rejects([](std::vector<TestSlice>& s) { s[1].b = -0.1; }, "slices[1].b must be >= 0");
    rejects([](std::vector<TestSlice>& s) { s[0].sigma = 0.0; }, "slices[0].sigma must be > 0");
    rejects([](std::vector<TestSlice>& s) { s[0].rho = -1.0; }, "slices[0].rho must be in (-1, 1)");
    rejects([](std::vector<TestSlice>& s) { s[1].F = 0.0; }, "slices[1].forward must be > 0");
    rejects(
        [](std::vector<TestSlice>& s) { s[1].a = -1.0; },
        "a + b sigma sqrt(1 - rho^2) must be >= 0");
    rejects(
        [](std::vector<TestSlice>& s) { s[1].expiryIso = "2026-10-16"; }, "strictly increasing");
    rejects(
        [](std::vector<TestSlice>& s) { s[0].expiryIso = "2026-09-30"; },
        "must be after reference_date");
}

}} // namespace quantra::testing
