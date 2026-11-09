// The arbitrage scan of /validate-vol-surface (checkLocalVol) is at least as
// strict as the structured note pricer: every local vol read a simulation
// makes (the note's time grid, which steps exactly on the expiries, and
// levels across and around the strike clamp's edges) goes through the same
// EdgeClampedLocalVol, and a read the pricer refuses is refused by the scan
// too.
//
// The surface reproduces the trap found on a real index sheet: two SVI
// slices one day apart whose later slice is quoted over a narrower strike
// range than the earlier one, and whose raw wing below that range crosses
// under the earlier slice (a calendar arbitrage in the unquoted wing only).
// Before 0.16.9 a read exactly at the first expiry used the first slice's
// wider range, its time stencil evaluated the second slice's wing and
// QuantLib refused ("decreasing variance"), while the scan, which never read
// at the expiry itself, reported nothing. Within 2e-4 years of an
// expiry the two ranges around it now intersect, so the step on the expiry
// never reads the second slice beyond its range: the note prices and the
// scan agrees; with the arbitrage inside the quoted
// ranges both refuse.

#include "basket_model.h"
#include "smile_slice_vol_surface.h"
#include "time_grid.h"
#include "vol_surface_validation_evaluator.h"

#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/volatility/equityfx/localvolsurface.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/nullcalendar.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace quantra { namespace testing {

namespace {

using QuantLib::Date;
using QuantLib::Handle;
using QuantLib::Real;
using QuantLib::Time;

struct TrapMarket {
    Date asOf{22, QuantLib::October, 2026};
    QuantLib::DayCounter dc = QuantLib::Actual365Fixed();
    Date d1 = asOf + 27, d2 = asOf + 28, noteEnd = asOf + 120;
    Handle<QuantLib::Quote> spot{std::make_shared<QuantLib::SimpleQuote>(100.0)};
    Handle<QuantLib::YieldTermStructure> zero{
        std::make_shared<QuantLib::FlatForward>(asOf, 0.0, dc)};
    Handle<QuantLib::BlackVolTermStructure> vol;

    TrapMarket() {
        // Slice 2 lies above slice 1 down to k = -0.1 (strike 90.5) and
        // below it further down the put wing.
        SviSliceParams p1{100.0, 0.00196, 0.02, 0.05, -0.7, 0.0};
        SviSliceParams p2{100.0, 0.00216, 0.02, 0.05, -0.6, 0.0};
        auto s = makeSviSurface(
            asOf, QuantLib::NullCalendar(), QuantLib::Unadjusted, dc, {d1, d2}, {p1, p2});
        s->enableExtrapolation();
        vol = Handle<QuantLib::BlackVolTermStructure>(s);
    }
    Time t1() const { return dc.yearFraction(asOf, d1); }
    Time t2() const { return dc.yearFraction(asOf, d2); }
};

struct Reads {
    unsigned reads = 0, refused = 0;
};

// Every read a daily-stepped simulation makes over levels 60 .. 160 and
// around every band edge, through the note's own clamped local vol.
Reads simulationReads(const TrapMarket& m, const std::vector<mc::LocalVolStrikeBand>& bands) {
    mc::BasketAsset asset = mc::MakeBasketAsset("NDX", m.spot, m.zero, m.zero, m.vol);
    asset.localVolStrikeBands = bands;
    asset.localVolTime = mc::LocalVolTimeExtrapolation::FlatLocalVol;
    mc::LocalVolCache cache;
    cache.attach(asset);
    const auto grid = mc::BuildSimulationGrid(m.asOf, {m.d1, m.d2, m.noteEnd}, 365);
    std::vector<Real> levels;
    for (int i = 0; i <= 200; ++i)
        levels.push_back(60.0 * std::pow(160.0 / 60.0, i / 200.0));
    for (const auto& b : bands)
        for (Real edge : {b.minStrike, b.maxStrike})
            for (Real d : {-2e-3, -1e-3, 0.0, 1e-3, 2e-3})
                levels.push_back(edge * std::exp(d));
    Reads r;
    for (std::size_t s = 0; s + 1 < grid.size(); ++s) {
        for (Real x : levels) {
            ++r.reads;
            try {
                asset.localVol->localVol(grid.grid[s], x, true);
            } catch (const std::exception&) {
                ++r.refused;
            }
        }
    }
    return r;
}

LocalVolCheckData scan(const TrapMarket& m, const std::vector<mc::LocalVolStrikeBand>& bands) {
    std::vector<std::pair<double, double>> regions;
    std::vector<double> times;
    for (const auto& b : bands) {
        times.push_back(b.time);
        regions.emplace_back(b.minStrike, b.maxStrike);
    }
    return checkLocalVol(
        m.vol, bands, std::nullopt, mc::LocalVolTimeExtrapolation::FlatLocalVol, m.spot, m.zero,
        m.zero, times, times, regions);
}

} // namespace

TEST(LocalVolScanCoverage, ReadsAtTheExpiryNeverLeaveTheNextSlicesRange) {
    TrapMarket m;
    // The trap is there: QuantLib's local vol at the first expiry at a level
    // inside the first slice's range but below the second's refuses.
    QuantLib::LocalVolSurface raw(m.vol, m.zero, m.zero, m.spot);
    EXPECT_THROW(raw.localVol(m.t1(), 85.0, true), std::exception);

    const std::vector<mc::LocalVolStrikeBand> bands = {
        {m.t1(), 80.0, 130.0}, {m.t2(), 93.0, 130.0}};
    mc::BasketAsset asset = mc::MakeBasketAsset("NDX", m.spot, m.zero, m.zero, m.vol);
    asset.localVolStrikeBands = bands;
    mc::LocalVolCache cache;
    cache.attach(asset);
    const auto* clamp =
        dynamic_cast<const mc::EdgeClampedLocalVol*>(asset.localVol.currentLink().get());
    ASSERT_NE(clamp, nullptr);
    const Real e = std::exp(mc::EdgeClampedLocalVol::kEdgeMargin);
    // Before the first expiry its range; within the stencil window around
    // it both ranges; between the expiries the second range.
    EXPECT_EQ(clamp->strikeBand(0.5 * m.t1()).first, 80.0 * e);
    EXPECT_EQ(clamp->strikeBand(m.t1()).first, 93.0 * e);
    EXPECT_EQ(clamp->strikeBand(m.t1() - 1e-4).first, 93.0 * e);
    EXPECT_EQ(clamp->strikeBand(0.5 * (m.t1() + m.t2())).first, 93.0 * e);
    EXPECT_EQ(clamp->clampedLevel(85.0, m.t1()), 93.0 * e);
    EXPECT_NO_THROW(asset.localVol->localVol(m.t1(), 85.0, true));

    const Reads sim = simulationReads(m, bands);
    EXPECT_GT(sim.reads, 20000u);
    EXPECT_EQ(sim.refused, 0u);
    const LocalVolCheckData c = scan(m, bands);
    EXPECT_EQ(c.refused, 0u) << c.firstRefusal;
    EXPECT_TRUE(c.arbitrageFree);
    // Scan times include each expiry itself.
    const auto times = localVolCheckTimes({m.t1(), m.t2()});
    EXPECT_NE(std::find(times.begin(), times.end(), m.t1()), times.end());
    EXPECT_NE(std::find(times.begin(), times.end(), m.t2()), times.end());
}

TEST(LocalVolScanCoverage, AnyRefusalOfTheSimulationIsARefusalOfTheScan) {
    TrapMarket m;
    // Ranges that reach into the crossing wing on both slices: the surface
    // is not arbitrage-free where the note reads, and both say so; with
    // ranges that stay above the crossing both pass.
    const std::vector<std::vector<mc::LocalVolStrikeBand>> cases = {
        {{m.t1(), 80.0, 130.0}, {m.t2(), 80.0, 130.0}},
        {{m.t1(), 88.0, 130.0}, {m.t2(), 85.0, 130.0}},
        {{m.t1(), 80.0, 130.0}, {m.t2(), 93.0, 130.0}},
        {{m.t1(), 95.0, 130.0}, {m.t2(), 93.0, 125.0}},
    };
    const std::vector<bool> refused = {true, true, false, false};
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Reads sim = simulationReads(m, cases[i]);
        const LocalVolCheckData c = scan(m, cases[i]);
        EXPECT_EQ(sim.refused > 0, refused[i]) << "case " << i;
        if (sim.refused > 0) {
            EXPECT_GT(c.refused, 0u) << "case " << i << ": the scan misses a refusal";
            EXPECT_FALSE(c.arbitrageFree) << "case " << i;
        }
        EXPECT_EQ(c.refused > 0, refused[i]) << "case " << i << ": " << c.firstRefusal;
    }
}

}} // namespace quantra::testing
