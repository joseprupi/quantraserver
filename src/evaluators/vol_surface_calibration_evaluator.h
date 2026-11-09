#ifndef QUANTRA_VOL_SURFACE_CALIBRATION_EVALUATOR_H
#define QUANTRA_VOL_SURFACE_CALIBRATION_EVALUATOR_H

/**
 * VolSurfaceCalibrationEvaluator — pure QuantLib core of `/calibrate-vol-surface`.
 *
 * From a sheet of listed European option quotes (the same OptionQuoteSheet as
 * `/calibrate-implied-forwards`) to a Black implied-volatility surface on a
 * strike x expiry grid, arbitrage-checked, with a report.
 *
 * 1. Forwards. Either the caller's per-expiry forward F and implied discount
 *    factor DF (the `expiries` array of an implied-forwards response) or, when
 *    absent, the implied-forward fit of ImpliedForwardsEvaluator run here
 *    with the same filter. Expiries without a forward are reported unusable.
 *
 * 2. Implied vols. Per expiry the per-line quality rules of the forward fit
 *    (filterExpiryLines) keep a quote line; the out-of-the-money side is
 *    used — the call for K >= F, the put for K < F — and its mid is inverted
 *    with Black-76 on the forward, blackFormulaImpliedStdDev(type, K, F,
 *    mid, DF), sigma = stdDev / sqrt(T), T = Act/365F from the as-of date.
 *    A mid below the quote resolution 1e-5 * S (0.24 points on a 24,000
 *    index: no vol information, any vol prices inside such a bid/ask) or a
 *    mid whose inversion does not solve (outside the Black bounds) is
 *    dropped with that reason.
 *
 * 3a. AndreasenHuge. Every used quote becomes a calibration pair
 *    (VanillaOption with the out-of-the-money payoff and the expiry, a
 *    SimpleQuote carrying its implied vol) of QuantLib's
 *    AndreasenHugeVolatilityInterpl (interpolation CubicSpline, calibration
 *    type CallPut, 500 grid points, Levenberg-Marquardt), on a risk-free
 *    curve with the implied discount factors (log-linear) and a dividend
 *    curve with DF_q(T) = F DF / S, so the model's forwards and discounting
 *    are the sheet's. The method calibrates a piecewise local volatility
 *    expiry by expiry to the Black prices of those vols on a one-step
 *    finite-difference scheme and is free of static arbitrage by
 *    construction (Andreasen & Huge, "Volatility Interpolation", 2010). The
 *    AndreasenHugeVolatilityAdapter then returns the Black vol at any grid
 *    (date, strike) by inverting the model price. A grid point is inverted
 *    only where the model's out-of-the-money price is at least 1e-4 * S (the
 *    one-step finite-difference price carries noise of ~1e-7 * S, which
 *    moves the implied vol by more than a tenth of a vol point below that
 *    and inverts to arbitrary vols where the true price is ~0); the other
 *    points are filled flat from the nearest inverted strike of the same
 *    expiry and the report counts them. No weights: the calibration matches
 *    prices.
 *
 * 3b. SviPerExpiry. Per usable expiry QuantLib's SviInterpolation
 *    (Levenberg-Marquardt, vega-weighted least squares of the implied vols,
 *    multiple deterministic starts) fits the raw SVI total variance
 *
 *        w(k) = a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2)),  k = ln(K / F).
 *
 *    At least 5 used quotes are needed. Each expiry has a region, where a
 *    simulation goes around it: 5 standard deviations of the log-forward
 *    on each side with the market's implied vols along the way, and the
 *    strikes quoted on both this expiry and the one before, within the
 *    output grid and the widest quoted range. Butterfly arbitrage:
 *    Durrleman's g(k) = (1 - k w'/(2w))^2 - (w'^2/4)(1/w + 1/4) + w''/2 on
 *    201 points of the output range and the regions of the slice's two
 *    expiry pairs, and a wing slope b (1 + |rho|) above Lee's bound 2, are
 *    violations; with enforce_butterfly the slice is refitted with a
 *    penalty 100 * max(0, -g(k_j)) per point within the bound.
 *
 *    Calendar: the slices are fitted from the last expiry back, each held
 *    under the next one. With total variance linear in time at fixed
 *    strike between the two expiries and Dupire's time derivative taken
 *    along the forward (x = ln(F_j / F_{j-1}), s = dw / d ln K), the
 *    derivative is non-negative over the interval iff, at every strike,
 *    w_j - w_{j-1} + x s_{j-1} >= m and w_j - w_{j-1} + x s_j >= m,
 *    m = min_forward_vol^2 (T_j - T_{j-1}) (pairBounds, 401 strikes over the
 *    later expiry's region). A slice that breaks it is refitted with the
 *    butterfly and calendar penalties together (from its fit and from the
 *    next slice moved to its forward), then lowered by any excess left.
 *    The report carries per expiry whether the refit acted, the largest
 *    vol change on the quotes, and after the fit the smallest margin and
 *    forward vol against the expiry before. Grid vols are sqrt(w(k) / T).
 *
 * 3c. Local vol check (checkLocalVol and quoteRegions of
 *    vol_surface_validation_evaluator.h, the /validate-vol-surface scan).
 *    Each returned spec is rebuilt as the structured
 *    note pricer builds it (BasketAsset + LocalVolCache, per-expiry strike
 *    bands = the regions, FlatLocalVol) on the sheet's forwards and
 *    QuantLib's LocalVolSurface is evaluated at every expiry, every expiry
 *    +/- 2e-4, the midpoints and T_last - 1e-3 on 201 strikes over the
 *    regions read at that time and at the clamp's edges (see checkLocalVol). A
 *    refusal makes arbitrage_free false; for the SVI payload the slices
 *    involved get a larger margin / butterfly weight and are refitted
 *    (at most 3 passes).
 *
 * 3d. Ssvi. The used quotes of every usable expiry (step 2: the same
 *    implied vols, forwards and discount factors) go to the eSSVI
 *    calibrator (src/market/ssvi_calibrator.h) with the expiry's forward
 *    and discount factor given, fitted short to long: theta solved from the
 *    quote nearest the money, (rho, psi) under the butterfly and calendar
 *    conditions, so the slices are free of static arbitrage by
 *    construction. Quotes beyond max_abs_log_moneyness are dropped with
 *    that reason (an expiry left with fewer than 3 is a 400). The grid is
 *    sampled from the eSSVI surface (makeSsviSurface on the sheet's spot
 *    and forwards; any grid date up to the last expiry), the slices are
 *    returned as an SsviVolSpec payload, and its closed-form local vol is
 *    checked as in 3c (the model's own continuation beyond the last slice,
 *    no refit: the slices are arbitrage-free). The payload carries no
 *    strike range (no slice strike_min / strike_max, no
 *    local_vol_strike_range): the formula is valid at every strike, so its
 *    local vol is read unclamped and the check reads it the same way.
 *
 * 4. Grid and calendar check. Strikes = moneyness * spot (default 0.50 ..
 *    1.50 step 0.025, 41 points, at least 2; Andreasen-Huge: the model's
 *    own finite-difference strikes within that range and the regions);
 *    expiries default to the fitted sheet expiries. At each node total
 *    variance is compared with the previous row's at the same forward
 *    moneyness; inside the expiry's region a node below it + 1e-8 is a
 *    violation, clipped when enforce_calendar (SviPerExpiry, a last
 *    resort), beyond it reported only.
 *
 * 5. Output. The surface is a BlackVolSpec SmileCube3D on a
 *    NullCalendar / Unadjusted / Actual365Fixed base with the grid expiries
 *    as calendar-day periods from the as-of date (so the dates are exact),
 *    read with surface_interpolator CubicStrikeLinearTimeForwardMoneyness
 *    (per expiry a natural cubic spline of total variance in log-strike,
 *    linear in total variance between expiries at fixed forward moneyness
 *    against the grid's expiry_forwards and the spot;
 *    src/market/smile_slice_vol_surface.h),
 *    extrapolation allowed, with per-expiry local-vol strike ranges (the
 *    regions inside the second and second-to-last grid strike). Every used
 *    quote is priced on that very object with fitQuotes (the fit test of
 *    /validate-vol-surface; kept on the result, not on the wire: the
 *    response carries one arbitrage_free flag per payload and the
 *    measurements are /validate-vol-surface's). With SviPerExpiry the fitted
 *    slices are returned as well, as an SviVolSpec payload with
 *    local_vol_strike_range PerExpiry and each slice's region as its
 *    strike_min / strike_max (the mapper builds it from `expiries[].svi`
 *    and `forwards`); the note's local vol reads it as
 *    mc::EdgeClampedLocalVol::strikeBand says (the range of the expiry
 *    closing the interval, intersected with the next one within 2e-4 of
 *    an expiry).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * vol_surface_calibration_mapper.{h,cpp}.
 */

#include "implied_forwards_evaluator.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "ssvi_calibrator.h"
#include "vol_surface_validation_evaluator.h"

#include <ql/time/date.hpp>
#include <ql/time/period.hpp>

#include <optional>
#include <string>
#include <vector>

namespace quantra {

enum class VolSurfaceFitMethod { AndreasenHuge, SviPerExpiry, Ssvi };

struct VolSurfaceCalibrationInputs {
    /// Sheet, filter, discounting curve and underlying (dividendCurveId unused).
    ImpliedForwardsInputs base;
    /// Present iff the request carried `forwards`.
    std::optional<std::vector<VolSurfaceForwardInput>> forwards;
    VolSurfaceFitMethod method = VolSurfaceFitMethod::AndreasenHuge;
    /// Empty = default grid.
    std::vector<double> moneyness;
    /// Empty = fitted sheet expiries.
    std::vector<QuantLib::Period> gridExpiries;
    std::string volSurfaceId;
    bool enforceButterfly = true;
    bool enforceCalendar = true;
    /// SviPerExpiry: the minimum forward volatility between two consecutive
    /// fitted slices (the calendar margin sigma_min^2 (T_i - T_{i-1}) in
    /// total variance), >= 0.
    double minForwardVol = 0.01;
    /// Ssvi: quote weighting and the |ln(K / F)| beyond which a used quote
    /// is left out of the fit (finite, > 0).
    ssvi::QuoteWeighting ssviWeighting = ssvi::QuoteWeighting::Vega;
    double ssviMaxAbsLogMoneyness = 1.0;
};

struct SviParams {
    double a = 0.0, b = 0.0, sigma = 0.0, rho = 0.0, m = 0.0;
};

struct VolSurfaceExpiryReportData {
    std::string expiryIso;
    QuantLib::Date expiry;
    double time = 0.0;
    bool usable = false;
    std::string droppedReason;
    std::optional<double> forward;
    std::optional<double> impliedDiscountFactor;
    unsigned quotesUsed = 0;
    std::vector<DroppedQuoteData> dropped;
    std::optional<double> rmseVol;
    std::optional<double> maxAbsErrorVol;
    std::optional<SviParams> svi;
    /// SviPerExpiry: the strike range checked around the expiry (the
    /// slice's strike_min / strike_max, local_vol_strike_range PerExpiry).
    /// Never set for Ssvi (the eSSVI payload is unclamped).
    std::optional<double> strikeMin, strikeMax;
    std::optional<double> butterflyMinG;
    bool butterflyViolation = false;
    std::string butterflyAction;
    /// SviPerExpiry, sequential fit (every fitted expiry after the first):
    /// whether the calendar penalty refit acted, the largest vol change it
    /// made over the slice's quoted strikes, and after the fit the smallest
    /// excess of total variance over the previous slice's plus the margin
    /// (>= 0 when satisfied) and the smallest forward vol between the two
    /// slices on the calendar grid.
    bool calendarRefit = false;
    std::optional<double> calendarRefitMaxVolShift;
    std::optional<double> calendarMinMargin;
    std::optional<double> calendarMinForwardVol;
    std::string calendarFitAction;
    unsigned calendarViolations = 0;
    std::optional<double> calendarMaxClipVol;
    std::string calendarAction;
    unsigned gridPointsFilled = 0;
    /// Ssvi: the fitted slice, its anchor (log-moneyness, total variance
    /// reproduced, whether it was raised for the calendar conditions) and
    /// the margins of the arbitrage conditions (calendar ones absent on the
    /// first slice).
    std::optional<SsviSliceParams> ssvi;
    std::optional<double> ssviAnchorK, ssviAnchorTotalVariance;
    bool ssviAnchorClamped = false;
    std::optional<double> ssviMarginButterflyPsi, ssviMarginButterflyPsi2;
    std::optional<double> ssviMarginCalendarTheta, ssviMarginCalendarPsi, ssviMarginCalendarA;
};

struct AndreasenHugeErrorData {
    double minVol = 0.0, maxVol = 0.0, avgVol = 0.0;
};

struct VolSurfaceCalibrationResult {
    std::string sheetId;
    std::string underlyingId;
    double spot = 0.0;
    VolSurfaceFitMethod method = VolSurfaceFitMethod::AndreasenHuge;
    std::string forwardsSource; // "request" | "fitted"
    QuoteFilterData filter;
    std::string asOfIso;
    std::vector<VolSurfaceForwardInput> forwards;     // fitted expiries, date order
    std::vector<VolSurfaceExpiryReportData> expiries; // every sheet expiry
    std::vector<double> moneyness;
    std::vector<double> gridStrikes;
    std::vector<std::string> gridExpiryIso;
    std::vector<int> gridExpiryDays; // calendar days from the as-of date
    /// Row-major by grid expiry then grid strike.
    std::vector<double> gridVols;
    std::optional<AndreasenHugeErrorData> andreasenHugeError;
    /// Per grid expiry: the strike range a note holds the grid's local vol
    /// to (BlackVolSpec local_vol_strike_min / _max).
    std::vector<double> gridLocalVolStrikeMin, gridLocalVolStrikeMax;
    /// Per grid expiry: the forward the grid row is read against
    /// (BlackVolSpec expiry_forwards): the fitted forward on a sheet
    /// expiry, spot D_q / D_r on the sheet's curves otherwise.
    std::vector<double> gridExpiryForwards;
    std::string volSurfaceId;
    /// The fit test of /validate-vol-surface (fitQuotes) on the returned grid
    /// with the sheet's forwards, default bands. Not on the wire: the
    /// endpoint returns surfaces; validation is /validate-vol-surface.
    FitValidationData fit;
    /// Local vol check (checkLocalVol, the /validate-vol-surface scan) of the
    /// grid surface (`vol_surface_spec`) and, for SviPerExpiry, of the SVI
    /// payload (`vol_surface_spec_svi`). Only `arbitrageFree` is on the wire.
    LocalVolCheckData localVolCheck;
    /// SviPerExpiry: what the payload's local vol check made the fit do
    /// ("none" or the refit passes).
    std::string sviLocalVolRefitAction;
    std::optional<LocalVolCheckData> localVolCheckSvi;
    /// Ssvi: the slices' arbitrage check and the local vol check of the
    /// SSVI payload (`vol_surface_spec_ssvi`, built by the mapper from
    /// `expiries[].ssvi`, `forwards` and `spot`).
    std::optional<SsviArbitrageCheckData> ssviArbitrage;
    std::optional<LocalVolCheckData> localVolCheckSsvi;
};

class VolSurfaceCalibrationEvaluator {
public:
    VolSurfaceCalibrationResult evaluate(
        const VolSurfaceCalibrationInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

/// Raw SVI total variance (exposed for tests).
double sviTotalVariance(const SviParams& p, double k);
/// Durrleman's butterfly function g(k) of a raw SVI slice (exposed for tests).
double sviDurrlemanG(const SviParams& p, double k);

} // namespace quantra

#endif // QUANTRA_VOL_SURFACE_CALIBRATION_EVALUATOR_H
