// Swaption smile cubes and SABR surfaces read outside their expiry grid keep
// the edge expiry's implied vol (flat in time), the convention of the SVI and
// cubic-strike Black surfaces. Inside the grid they interpolate total
// variance between the bracketing expiries, unchanged.

#include "parity_fixture.h"

#include <cmath>
#include <vector>

namespace quantra { namespace testing {

namespace {

// Referenced on 2025-01-15 (TARGET, ModifiedFollowing, Actual365Fixed): the
// 1Y and 2Y expiries sit at times 1.0 and 2.0.
const std::vector<QuantLib::Period> kExpiries = {
    QuantLib::Period(1, QuantLib::Years), QuantLib::Period(2, QuantLib::Years)};
const std::vector<QuantLib::Period> kTenors = {
    QuantLib::Period(5, QuantLib::Years), QuantLib::Period(10, QuantLib::Years)};
const std::vector<double> kStrikes = {0.01, 0.03, 0.05};

// 1M and 3M before the grid, 3Y and 5Y after it.
const std::vector<double> kBeforeGrid = {1.0 / 12.0, 0.25};
const std::vector<double> kAfterGrid = {3.0, 5.0};

const double kSwapLength = 7.0;

SwaptionVolEntry parseSurface(
    flatbuffers::grpc::MessageBuilder& b, flatbuffers::Offset<quantra::VolSurfaceSpec> spec) {
    b.Finish(spec);
    return parseSwaptionVol(
        flatbuffers::GetRoot<quantra::VolSurfaceSpec>(b.GetBufferPointer()), nullptr);
}

// Smile cube vols: `first` at the 1Y expiry, `second` at 2Y, any tenor/strike.
std::vector<double> cubeVols(double first, double second) {
    std::vector<double> vols;
    for (std::size_t i = 0; i < kExpiries.size(); ++i)
        for (std::size_t j = 0; j < kTenors.size() * kStrikes.size(); ++j)
            vols.push_back(i == 0 ? first : second);
    return vols;
}

} // namespace

TEST_F(QuantraComparisonTest, SwaptionSmileCube_FlatCubeIsFlatOutsideTheExpiryGrid) {
    flatbuffers::grpc::MessageBuilder b;
    const auto entry = parseSurface(
        b, buildSwaptionVolSmileCubeSurface(
               b, "flat_cube", kExpiries, kTenors, kStrikes, cubeVols(0.20, 0.20),
               quantra::enums::SwaptionStrikeKind_Absolute, "EUR_SWAP_6M"));
    ASSERT_FALSE(entry.handle.empty());

    std::vector<double> times = kBeforeGrid;
    times.insert(times.end(), kAfterGrid.begin(), kAfterGrid.end());
    for (double t : times) {
        for (double k : kStrikes) {
            EXPECT_NEAR(entry.handle->volatility(t, kSwapLength, k, true), 0.20, 1e-14)
                << "t=" << t << " k=" << k;
        }
        // The smile section a CMS pricer reads gives the same vol.
        EXPECT_NEAR(entry.handle->smileSection(t, kSwapLength, true)->volatility(0.03), 0.20, 1e-14)
            << "smile section t=" << t;
    }
}

TEST_F(QuantraComparisonTest, SwaptionSmileCube_EdgeExpiryVolOutsideTheGrid) {
    flatbuffers::grpc::MessageBuilder b;
    const auto entry = parseSurface(
        b, buildSwaptionVolSmileCubeSurface(
               b, "term_cube", kExpiries, kTenors, kStrikes, cubeVols(0.20, 0.30),
               quantra::enums::SwaptionStrikeKind_Absolute, "EUR_SWAP_6M"));
    ASSERT_FALSE(entry.handle.empty());

    for (double t : kBeforeGrid) {
        EXPECT_NEAR(entry.handle->volatility(t, kSwapLength, 0.03, true), 0.20, 1e-14) << "t=" << t;
    }
    for (double t : kAfterGrid) {
        EXPECT_NEAR(entry.handle->volatility(t, kSwapLength, 0.03, true), 0.30, 1e-14) << "t=" << t;
    }
    // Inside the grid: total variance linear between the 1Y and 2Y nodes.
    const double t = 1.5, w = 0.5;
    const double expected = std::sqrt((0.20 * 0.20 * 1.0 * (1 - w) + 0.30 * 0.30 * 2.0 * w) / t);
    EXPECT_NEAR(entry.handle->volatility(t, kSwapLength, 0.03, true), expected, 1e-14);
}

TEST_F(QuantraComparisonTest, SwaptionSabrParams_FlatOutsideTheExpiryGrid) {
    // beta 1, rho 0 and a vanishing nu: every node's smile is flat at alpha (at
    // the money exactly; away from it to the SABR formula's rounding, which is
    // why the other strikes are checked against the edge expiry's own vol).
    const std::size_t nodes = kExpiries.size() * kTenors.size();
    const std::vector<double> alpha(nodes, 0.20), beta(nodes, 1.0), rho(nodes, 0.0),
        nu(nodes, 1e-8);
    const double forward = 0.03;
    flatbuffers::grpc::MessageBuilder b;
    const auto parsed = parseSurface(
        b,
        buildSwaptionSabrParamsSurface(b, "flat_sabr", kExpiries, kTenors, alpha, beta, rho, nu));
    const auto entry = withSwaptionSabrParamsAtm(parsed, std::vector<double>(nodes, forward));
    ASSERT_FALSE(entry.handle.empty());
    const auto vol = [&](double t, double k) {
        return entry.handle->volatility(t, kSwapLength, k, true);
    };

    for (double t : kBeforeGrid) {
        EXPECT_NEAR(vol(t, forward), 0.20, 1e-14) << "t=" << t;
        for (double k : kStrikes)
            EXPECT_DOUBLE_EQ(vol(t, k), vol(1.0, k)) << "t=" << t << " k=" << k;
    }
    for (double t : kAfterGrid) {
        EXPECT_NEAR(vol(t, forward), 0.20, 1e-14) << "t=" << t;
        for (double k : kStrikes)
            EXPECT_DOUBLE_EQ(vol(t, k), vol(2.0, k)) << "t=" << t << " k=" << k;
    }
}

}} // namespace quantra::testing
