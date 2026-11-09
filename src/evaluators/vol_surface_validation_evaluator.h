#ifndef QUANTRA_VOL_SURFACE_VALIDATION_EVALUATOR_H
#define QUANTRA_VOL_SURFACE_VALIDATION_EVALUATOR_H

/**
 * VolSurfaceValidationEvaluator — pure QuantLib core of `/validate-vol-surface`,
 * and the one implementation of each surface test (the calibrate endpoint
 * runs the same functions on the surfaces it returns).
 *
 * The surface is the registry's Black vol `volatility_id`, exactly as the
 * parser built it for the pricers (any shape: a strike x expiry grid, an SVI
 * or SSVI payload, a constant or term vol). Nothing is fitted.
 *
 * Market. Without `forwards`: spot and dividend curve of `underlying_id`,
 * risk-free curve `discounting_curve`; per expiry T (Act/365F from the as-of
 * date) DF = D(T), F = S D_div(T) / D(T) — the market a structured note
 * simulates on. With `forwards` (an implied-forwards response pasted): per
 * listed expiry the sheet's F and DF, and the curves the calibrate endpoint
 * fits on (sheetForwardCurves: risk-free on the DFs, dividend DF_q = F DF / S,
 * DiscountCurve log-linear); unlisted expiries are dropped.
 *
 * Quotes. Per expiry the per-line rules of the forward fit and the
 * out-of-the-money side with its Black-76 implied vol
 * (selectOutOfTheMoneyQuotes, as `/calibrate-vol-surface`); an expiry with
 * fewer than `filter.min_quotes_per_expiry` used quotes is reported and left
 * out of every test.
 *
 * Fit (fitQuotes). Every used quote priced with Black-76 at the surface's
 * own vol, blackVol(expiry, K, extrapolate), on the expiry's F and DF;
 * inside the bid/ask, vol and price errors, the band summary
 * (vol_surface_band_summary.h), the at-the-money error per expiry (the quote
 * with the smallest |ln(K / F)|) and a verdict: pass when >= 90 % inside and
 * every |ATM error| <= 0.005, fail below 50 % inside, warn otherwise.
 *
 * Arbitrage (checkLocalVol). The surface's local vol built through the note
 * pricer's own code path (BasketAsset + LocalVolCache: QuantLib's
 * LocalVolSurface, or SsviLocalVol for an SSVI payload, held at the spec's
 * strike limits and extended in time as the spec says), evaluated at
 * localVolCheckTimes(surface expiries) x 201 log-spaced strikes over the
 * union of the quote regions (quoteRegions) of the times within the
 * stencil window of each time (the first usable expiry at or after it; both
 * around an expiry), plus the clamp's edges and the points 1e-3 in
 * log-strike either side of them. A refusal (QuantLib throws) fails the
 * check. An SVI payload adds
 * per slice Durrleman's g on 201 points of its strike range, the wing slope
 * against Lee's bound 2 and the smallest total variance increase at the same
 * log-moneyness against the previous slice; an SSVI payload the closed-form
 * conditions (ssvi::checkArbitrage), the numeric scan (ssvi::defaultScan)
 * and per slice the condition margins.
 *
 * Simulation (simulateQuotes). One asset built exactly as a structured note
 * builds it (MakeBasketAsset on spot, dividend, risk-free and the surface;
 * the spec's strike limits and time extension; the Monte Carlo spec's
 * local_vol_floor), simulated with the note's path engine on the grid of the
 * used expiries (BuildSimulationGrid, refined by steps_per_year); every used
 * quote is the discounted mean of its out-of-the-money payoff at its expiry
 * point, with the path engine's standard error; mc_vol is its Black-76
 * implied vol. Verdict: pass when >= 95 % are inside the bid/ask widened by
 * 3.5 standard errors, fail below 80 %, warn otherwise. A single-underlying
 * note whose observation dates are among the used expiries draws the same
 * stream on the same grid, so its former vanilla repricing is reproduced to
 * the bit.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces (scripts/check_evaluator_boundary.sh).
 */

#include "basket_model.h"
#include "implied_forwards_evaluator.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "structured_note_data.h"
#include "vol_surface_band_summary.h"

#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace quantra {

/// One forward supplied by the caller (from an implied-forwards response).
struct VolSurfaceForwardInput {
    std::string expiryIso;
    QuantLib::Date expiry;
    double time = 0.0;
    double forward = 0.0;
    double impliedDiscountFactor = 0.0;
};

// ---------------------------------------------------------------------------
// Shared pieces (also run by the calibrate endpoint on the surfaces it returns)
// ---------------------------------------------------------------------------

/// Dupire local vol of a surface on a scan of times and strikes.
struct LocalVolCheckData {
    unsigned times = 0;
    unsigned strikes = 0;
    unsigned points = 0;
    /// Scan points where the local vol refused (exception).
    unsigned refused = 0;
    std::string firstRefusal;
    /// Smallest local variance over the points that evaluated, and where.
    std::optional<double> minLocalVariance;
    std::optional<double> minLocalVarianceTime;
    std::optional<double> minLocalVarianceStrike;
    bool arbitrageFree = false;
};

/// Every point of a local vol scan (opt-in): one row per scan time, the
/// strikes of that row (they follow the row's region), the local vol at each
/// (0 where refused) and the indices of the refused strikes.
struct LocalVolGridRowData {
    double time = 0.0;
    std::vector<double> strikes;
    std::vector<double> localVols;
    std::vector<unsigned> refused;
};
struct LocalVolGridData {
    std::vector<LocalVolGridRowData> rows;
};

/// A strike clamp the note's local vol applies when reading the surface:
/// per expiry (the spec's per-expiry ranges: the edges a simulation step AT
/// that expiry reads at: its range intersected with the next expiry's,
/// see mc::EdgeClampedLocalVol::strikeBand) or surface-wide (one
/// entry per surface expiry, or one without expiry when the surface has
/// none); each edge is the level the local vol is read at beyond it (the
/// range moved inside by EdgeClampedLocalVol::kEdgeMargin in log-strike),
/// absent on an open side.
struct ClampEdgeData {
    std::string scope; // "PerExpiry" | "Surface"
    std::optional<std::string> expiryIso;
    std::optional<double> time;
    std::optional<double> strikeMin, strikeMax;
};

/// eSSVI closed-form conditions found violated and the numeric scan.
struct SsviArbitrageCheckData {
    std::vector<std::string> violations;
    double scanMinG = 0.0, scanMinGTime = 0.0, scanMinGK = 0.0;
    double scanMinDwDt = 0.0, scanMinDwDtTime = 0.0, scanMinDwDtK = 0.0;
};

/// The local vol settings a structured note takes from a vol spec: the
/// strike range (`localVolStrikeRange`), the per-expiry strike bands (times
/// from the vol's reference date) and the time extension.
void applyLocalVolSettings(const BlackVolEntry& entry, mc::BasketAsset& asset);

/// The local vol time extension the vol spec asks for (FlatImpliedVol when
/// `entry` is null).
mc::LocalVolTimeExtrapolation localVolTimeOf(const BlackVolEntry* entry);

/// Scan times: every expiry, every expiry +/- 2e-4, the midpoints (and
/// between 0 and the first), and the last expiry - 1e-3 (where FlatLocalVol
/// reads).
std::vector<double> localVolCheckTimes(const std::vector<double>& expiryTimes);

/**
 * The local vol a structured note derives from `surface` (BasketAsset +
 * LocalVolCache, with `bands` / `strikeRange` / `timeExtension` applied as
 * the note applies them; a constant or term vol: QuantLib's LocalVolSurface
 * on it), evaluated at localVolCheckTimes(expiryTimes) x 201 strikes
 * log-spaced over the union of `regions[r]` for r the first
 * `regionTimes[r]` >= x (the last beyond), x every time within
 * EdgeClampedLocalVol::kBandTimeWindow of t (one region inside an interval,
 * both around an expiry), plus, when the local vol is clamped, the clamp's edges at the
 * time read and the strikes 1e-3 in log-strike either side of each.
 * `refusedTimes` (optional) receives each refused time with whether the
 * message was a calendar one ("decreasing variance").
 */
LocalVolCheckData checkLocalVol(
    const QuantLib::Handle<QuantLib::BlackVolTermStructure>& surface,
    const std::vector<mc::LocalVolStrikeBand>& bands,
    const std::optional<std::pair<double, double>>& strikeRange,
    mc::LocalVolTimeExtrapolation timeExtension,
    const QuantLib::Handle<QuantLib::Quote>& spot,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFree,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& dividend,
    const std::vector<double>& expiryTimes,
    const std::vector<double>& regionTimes,
    const std::vector<std::pair<double, double>>& regions,
    std::vector<std::pair<double, bool>>* refusedTimes = nullptr,
    LocalVolGridData* grid = nullptr);

/// One expiry's used quotes for the region rule.
struct RegionQuotes {
    double time = 0.0;
    double forward = 0.0;
    const std::vector<OutOfTheMoneyQuote>* used = nullptr; // sorted by strike, non-empty
};

/**
 * The strike region of each expiry (date order): from the forward outwards
 * on each side, the log-strike distance covered by five standard deviations
 * when the vol along the way is the quotes' implied vol (trapezoid of 1 /
 * sigma over the quoted strikes, the edge vol beyond them), the larger of
 * this expiry's and the previous one's reach; extended to the strikes quoted
 * on both; then held inside [clampLo, clampHi].
 */
std::vector<std::pair<double, double>> quoteRegions(
    const std::vector<RegionQuotes>& expiries, double clampLo, double clampHi);

/// The sheet's market: a risk-free DiscountCurve on the implied discount
/// factors and a dividend curve DF_q = F DF / S (Act/365F, from `asOf`).
struct SheetForwardCurves {
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree;
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend;
};
SheetForwardCurves sheetForwardCurves(
    const QuantLib::Date& asOf,
    double spot,
    const std::vector<QuantLib::Date>& expiries,
    const std::vector<double>& forwards,
    const std::vector<double>& discountFactors);

/// One sheet expiry as the tests use it.
struct ValidationExpiry {
    std::string expiryIso;
    QuantLib::Date expiry;
    double time = 0.0;
    bool usable = false;
    std::string droppedReason;
    std::optional<double> forward;
    std::optional<double> discountFactor;
    std::vector<OutOfTheMoneyQuote> used; // sorted by strike
    std::vector<DroppedQuoteData> dropped;
};

struct QuoteFitData {
    std::string expiryIso;
    double time = 0.0;
    double strike = 0.0;
    double moneyness = 0.0;
    bool isCall = true;
    double bid = 0.0, ask = 0.0, mid = 0.0;
    double marketVol = 0.0;
    /// Black-76 implied vols of the bid and the ask (same forward, discount
    /// factor and side as marketVol); absent when the price has none.
    std::optional<double> bidVol, askVol;
    double modelVol = 0.0;
    double modelPrice = 0.0;
    bool insideSpread = false;
    double volError = 0.0;
    double priceError = 0.0;
};

struct AtmErrorData {
    std::string expiryIso;
    double time = 0.0;
    double strike = 0.0;
    double moneyness = 0.0;
    double marketVol = 0.0;
    double modelVol = 0.0;
    double volError = 0.0;
};

struct FitValidationData {
    unsigned count = 0;
    unsigned inside = 0;
    double rmsVolError = 0.0;
    double maxAbsVolError = 0.0;
    double maxAbsPriceError = 0.0;
    std::vector<QuoteFitData> quotes;
    std::vector<BandCell> bands;
    std::vector<AtmErrorData> atm;
    std::string verdict;
};

/// The fit test on the usable entries of `expiries` (date order).
FitValidationData fitQuotes(
    const QuantLib::BlackVolTermStructure& surface,
    const std::vector<ValidationExpiry>& expiries,
    const VolSurfaceBandEdges& edges);

// ---------------------------------------------------------------------------
// The endpoint
// ---------------------------------------------------------------------------

struct VolSurfaceValidationInputs {
    std::string volatilityId;
    std::string underlyingId;
    std::string discountingCurveId;
    /// Echo only (the mapper reads it from the request's underlying spec).
    std::string dividendCurveId;
    std::string sheetId;
    std::string sheetUnderlyingId;
    OptionSheetStyle style = OptionSheetStyle::European;
    std::vector<OptionQuoteLine> quotes;
    QuoteFilterData filter;
    std::optional<std::vector<VolSurfaceForwardInput>> forwards;
    bool checkFit = true;
    bool checkArbitrage = true;
    bool checkSimulation = false;
    /// With checkArbitrage: return every scanned point (LocalVolGridData).
    bool checkArbitrageGrid = false;
    std::optional<MonteCarloData> simulation;
    /// Requested edges (empty axis = default).
    VolSurfaceBandEdges bands;
};

struct SviSliceCheckData {
    std::string expiryIso;
    double time = 0.0;
    double butterflyMinG = 0.0, butterflyMinGK = 0.0;
    double wingSlope = 0.0;
    std::optional<double> calendarMinDw, calendarMinDwK, calendarMinForwardVol;
};

struct SsviSliceCheckData {
    std::string expiryIso;
    double time = 0.0;
    double theta = 0.0, rho = 0.0, psi = 0.0;
    double marginButterflyPsi = 0.0, marginButterflyPsi2 = 0.0;
    std::optional<double> marginCalendarTheta, marginCalendarPsi, marginCalendarA;
};

struct ArbitrageValidationData {
    bool arbitrageFree = false;
    LocalVolCheckData localVol;
    /// Present iff checks.arbitrage_grid.
    std::optional<LocalVolGridData> grid;
    /// The strike clamp the note's local vol applies; empty when the surface
    /// is read unclamped (a constant or term vol, a payload without strike
    /// limits).
    std::vector<ClampEdgeData> clampEdges;
    std::optional<std::vector<SviSliceCheckData>> sviSlices;
    std::optional<SsviArbitrageCheckData> ssvi;
    std::optional<std::vector<SsviSliceCheckData>> ssviSlices;
    std::vector<std::string> violations;
    std::string verdict;
};

struct QuoteSimulationData {
    std::string expiryIso;
    double time = 0.0;
    double strike = 0.0;
    double moneyness = 0.0;
    bool isCall = true;
    double bid = 0.0, ask = 0.0, mid = 0.0;
    double marketVol = 0.0;
    double mcPrice = 0.0;
    double standardError = 0.0;
    std::optional<double> mcVol;
    bool insideSpread = false;
    bool withinNoise = false;
    std::optional<double> volError;
    double priceError = 0.0;
};

struct SimulationValidationData {
    unsigned paths = 0;
    unsigned timeSteps = 0;
    unsigned long seed = 0;
    unsigned count = 0;
    unsigned inside = 0;
    unsigned withinNoise = 0;
    double maxAbsPriceError = 0.0;
    std::optional<double> maxAbsVolError;
    double maxStandardError = 0.0;
    std::vector<QuoteSimulationData> quotes;
    std::vector<BandCell> bands;
    std::string verdict;
};

struct VolSurfaceValidationResult {
    std::string volatilityId;
    quantra::enums::VolSurfaceShape shape = quantra::enums::VolSurfaceShape_Constant;
    std::string underlyingId;
    double spot = 0.0;
    std::string discountingCurveId;
    std::string dividendCurveId;
    std::string forwardsSource; // "request" | "curves"
    std::string sheetId;
    QuoteFilterData filter;
    bool checkFit = true, checkArbitrage = true, checkSimulation = false;
    bool checkArbitrageGrid = false;
    VolSurfaceBandEdges bands;
    std::vector<ValidationExpiry> expiries;
    std::optional<FitValidationData> fit;
    std::optional<ArbitrageValidationData> arbitrage;
    std::optional<SimulationValidationData> simulation;
    std::optional<double> fitMilliseconds, arbitrageMilliseconds, simulationMilliseconds;
    double totalMilliseconds = 0.0;
};

class VolSurfaceValidationEvaluator {
public:
    VolSurfaceValidationResult evaluate(
        const VolSurfaceValidationInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_VOL_SURFACE_VALIDATION_EVALUATOR_H
