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
 *    multiple deterministic starts, the parameter transforms keeping b >= 0,
 *    |rho| < 1, sigma > 0, a + b sigma sqrt(1 - rho^2) >= 0 and
 *    b (1 + |rho|) <= 4) fits the raw SVI total variance
 *
 *        w(k) = a + b (rho (k - m) + sqrt((k - m)^2 + sigma^2)),  k = ln(K / F).
 *
 *    At least 5 used quotes are needed. Butterfly arbitrage: Durrleman's
 *    g(k) = (1 - k w'/(2w))^2 - (w'^2/4)(1/w + 1/4) + w''/2 is evaluated on
 *    201 points of the output strike range; min g < 0 is a violation. With
 *    enforce_butterfly the slice is refitted from those parameters by
 *    Levenberg-Marquardt on the same weighted residuals plus a penalty
 *    100 * max(0, -g(k_j)) per point, and g is re-evaluated; the report
 *    carries min g before and after. Grid vols are sqrt(w(k) / T).
 *
 * 4. Grid and calendar check. Strikes = moneyness * spot (default 0.50 ..
 *    1.50 step 0.025, 41 points, at least 2); expiries default to the
 *    fitted sheet expiries. At each
 *    grid strike total variance w_i = sigma_i^2 T_i must not decrease with
 *    the expiry; a point where w_i < w_{i-1} + 1e-8 is a calendar violation,
 *    clipped to w_{i-1} + 1e-8 when enforce_calendar (SviPerExpiry) and
 *    reported for both methods.
 *
 * 5. Output and repricing. The surface is a BlackVolSpec SmileCube3D on a
 *    NullCalendar / Unadjusted / Actual365Fixed base with the grid expiries
 *    as calendar-day periods from the as-of date (so the dates are exact),
 *    read with surface_interpolator CubicStrikeLinearTime (per expiry a
 *    natural cubic spline of total variance in log-strike, linear in total
 *    variance between expiries; src/market/smile_slice_vol_surface.h),
 *    extrapolation allowed. Every used quote is repriced on that very
 *    object (SmileSliceVolSurface, as vol_surface_parsers builds it): model
 *    vol at (expiry, K), Black price on the same F and DF, inside the
 *    bid/ask or not, with the largest price and vol excursions. With
 *    SviPerExpiry the fitted slices (forward, a, b, sigma, rho, m per
 *    usable expiry, after the butterfly refit, before the grid's calendar
 *    clip) are returned as well, as an SviVolSpec payload (the mapper
 *    builds it from `expiries[].svi` and `forwards`).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * vol_surface_calibration_mapper.{h,cpp}.
 */

#include "implied_forwards_evaluator.h"
#include "pricing_context.h"
#include "pricing_registry.h"

#include <ql/time/date.hpp>
#include <ql/time/period.hpp>

#include <optional>
#include <string>
#include <vector>

namespace quantra {

enum class VolSurfaceFitMethod { AndreasenHuge, SviPerExpiry };

/// One forward supplied by the caller (from an implied-forwards response).
struct VolSurfaceForwardInput {
    std::string expiryIso;
    QuantLib::Date expiry;
    double time = 0.0;
    double forward = 0.0;
    double impliedDiscountFactor = 0.0;
};

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
    std::optional<double> butterflyMinG;
    bool butterflyViolation = false;
    std::string butterflyAction;
    unsigned calendarViolations = 0;
    std::optional<double> calendarMaxClipVol;
    std::string calendarAction;
    unsigned gridPointsFilled = 0;
};

struct VanillaRepricePointData {
    std::string expiryIso;
    double strike = 0.0;
    bool isCall = true;
    double bid = 0.0, ask = 0.0, mid = 0.0;
    double marketVol = 0.0;
    double modelVol = 0.0;
    double modelPrice = 0.0;
    bool insideBidAsk = false;
    double priceError = 0.0;
    double volError = 0.0;
};

struct VanillaRepricingData {
    unsigned count = 0;
    unsigned insideBidAsk = 0;
    double maxAbsPriceError = 0.0;
    double maxAbsVolError = 0.0;
    std::vector<VanillaRepricePointData> quotes;
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
    std::string volSurfaceId;
    VanillaRepricingData repricing;
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
