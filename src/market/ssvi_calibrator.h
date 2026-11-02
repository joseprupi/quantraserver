#ifndef QUANTRA_SSVI_CALIBRATOR_H
#define QUANTRA_SSVI_CALIBRATOR_H

/**
 * eSSVI slice calibration from option quotes (Corbetta, Cohort, Laachir, Martini 2019,
 * "Robust calibration and arbitrage-free interpolation of SSVI slices", arXiv:1804.04924,
 * Sec. 2-3). The result is arbitrage free by construction (butterfly and calendar conditions of
 * ssvi.h hold slice by slice) and is checked again with checkArbitrage and a numeric scan.
 *
 * Pipeline
 *   1. Quotes are grouped by expiry, T = dayCounter.yearFraction(reference, expiry) > 0.
 *   2. Per expiry the forward is, when calls AND priced puts exist at >= 2 common strikes, the
 *      put-call-parity estimate F = sum_j w_j ((C_j - P_j) / D + K_j) / sum_j w_j with D =
 *      discount(T) and w_j = 1 (equal weights: every common strike is an unbiased estimate of F);
 *      otherwise the curve forward F = spot D_q(T) / D_r(T) (400 when there is no dividend
 *      curve). Both are reported, with the implied
 *      dividend yield q = -ln(F D_r(T) / spot) / T.
 *   3. Implied total variance per quote: vol^2 T for a vol quote, the squared Black implied
 *      standard deviation for a price quote (QUANTRA_INVALID_ARGUMENT naming the quote when the
 *      price is outside the no-arbitrage bounds). Where a call and a put exist at one strike
 *      the out-of-the-money one (call if K >= F) is used. k = ln(K / F).
 *   3b. A bad quote is excluded and reported (QuoteRow::exclusionReason), not a 400: price
 *      outside the no-arbitrage bounds at the slice forward, parity outlier (pairs further than
 *      max(5 * 1.4826 MAD, 1e-4 F_med) from the median pair forward when there are >= 3 pairs,
 *      both quotes dropped; the forward is the mean of the kept pairs), failed implied-vol solve,
 *      implied variance not > 0, ask <= bid under BidAsk weighting. A slice with fewer than 3 used
 *      quotes is a 400 naming the expiry, the count and the exclusions by reason.
 *   4. Slices are fitted short to long. The anchor (k*, W) is the used quote with the smallest
 *      |k| and its market total variance. For (rho, psi) the slice variance is the exact
 *      inversion of w(k*; theta, rho, psi) = W (squaring 2W - theta - rho psi k* =
 *      sqrt(psi^2 k*^2 + 2 rho psi theta k* + theta^2); the root is unique because
 *      2W - theta - rho psi k* = W + c psi^2 > 0):
 *        theta(psi) = W - rho k* psi - c psi^2,   c = (1 - rho^2) k*^2 / (4 W) >= 0.
 *      The model therefore reproduces the anchor quote exactly and (rho, psi) is what is
 *      searched. For a given rho every constraint is a quadratic in psi with a nonnegative
 *      leading coefficient, so each feasible set is an interval:
 *        theta(psi) > 0, butterfly psi (1 + |rho|) <= 4 and psi^2 (1 + |rho|) <= 4 theta(psi),
 *        calendar theta(psi) > theta_prev and psi >= psi_prev max((1 - rho_prev) / (1 - rho),
 *        (1 + rho_prev) / (1 + rho)),
 *      and the calendar condition is made sufficient by also requiring phi = psi / theta to be
 *      non-increasing (Hendriks-Martini Prop. 3.5 (i), see ssvi.h): psi theta_prev <= psi_prev
 *      theta(psi).
 *      Strict inequalities keep a relative margin 1e-10. For the first slice theta_prev =
 *      psi_prev = 0 (psi is then bounded below by 1e-9 upper).
 *      The objective is sum_j weight_j omega_j (sigma_model(k_j) - sigma_mkt_j)^2 with omega = 1
 *      (Uniform), the Black vega at the market vol (Vega) or 1 / spread^2 in vol units (BidAsk;
 *      a price spread is divided by the vega, quotes without bid/ask take the slice's median
 *      spread). psi is minimised for each rho by Brent's method (golden section + parabolic
 *      interpolation, 1e-12 relative). rho goes through a 41-point grid on [-0.999, 0.999]
 *      (plus rho_prev, which is always feasible for a clamped anchor) and is then refined on
 *      21-point grids of half-width two spacings around the best value, the spacing shrinking by
 *      5 each round until it is below 1e-10 (the specification of three rounds stops at
 *      4e-4, too coarse to recover the parameters to 1e-6).
 *      If no rho is feasible (calendar arbitrage in the data at the money) theta* is raised to
 *      theta_prev (1 + 1e-6) + 1e-12 and anchorClamped is set; if still infeasible the call
 *      fails naming the expiry.
 *   5. checkArbitrage must be empty (else QUANTRA_ERROR) and a numeric scan runs on the slice
 *      times, their midpoints, 0.5 T_1 and 1.5 T_N for k in [-1.5, 1.0] (201 points).
 */

#include "ssvi.h"
#include "ssvi_vol_surface.h"

#include <ql/handle.hpp>
#include <ql/option.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace quantra { namespace ssvi {

enum class QuoteWeighting { Uniform, Vega, BidAsk };

// Exactly one of price / impliedVol. bid / ask are in the units of the value given.
struct OptionQuote {
    QuantLib::Date expiry;
    double strike = 0.0;
    QuantLib::Option::Type type = QuantLib::Option::Call;
    std::optional<double> price;
    std::optional<double> impliedVol;
    std::optional<double> bid;
    std::optional<double> ask;
    double weight = 1.0;
};

struct CalibrationInput {
    QuantLib::Date referenceDate;
    QuantLib::DayCounter dayCounter;
    double spot = 0.0;
    QuantLib::Handle<QuantLib::YieldTermStructure> discount;
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend; // may be empty: parity forwards only
    std::vector<OptionQuote> quotes;
    QuoteWeighting weighting = QuoteWeighting::Uniform;
    double maxAbsLogMoneyness = 1.0;
};

struct QuoteRow {
    std::size_t quoteIndex = 0;
    QuantLib::Date expiry;
    double strike = 0.0;
    double k = 0.0;
    double marketVol = 0.0; // NaN when the implied vol of an unused quote cannot be solved
    double modelVol = 0.0;
    double residualVolPts = 0.0; // (model - market) * 100
    bool used = false;
    // Empty for used quotes. Otherwise one of: price_outside_bounds, parity_outlier,
    // implied_vol_not_solved, in_the_money_side, max_abs_log_moneyness,
    // implied_vol_not_positive, ask_not_above_bid.
    std::string exclusionReason;
};

struct SliceDiagnostics {
    std::size_t usedQuotes = 0;
    double rmseVolPts = 0.0;
    double maxAbsVolPts = 0.0;
    double anchorK = 0.0;
    double anchorTheta = 0.0;
    bool anchorClamped = false;
    std::size_t parityPairsUsed = 0;     // put-call pairs behind the parity forward
    std::size_t parityPairsRejected = 0; // pairs dropped as outliers
    // rhs - lhs of each condition, >= 0 when satisfied. Calendar margins of the first slice
    // are +infinity.
    double marginButterflyPsi = 0.0;  // 4 - psi (1 + |rho|)
    double marginButterflyPsi2 = 0.0; // 4 theta - psi^2 (1 + |rho|)
    double marginCalendarTheta = 0.0; // theta_i - theta_{i-1}
    double marginCalendarPsi = 0.0;   // psi_i - psi_{i-1}
    double marginCalendarA = 0.0;     // (psi_i - psi_{i-1}) - |a_i - a_{i-1}|
};

struct CalibrationResult {
    std::vector<QuantLib::Date> expiries;
    std::vector<SsviSliceParams> params;
    std::vector<double> times;
    std::vector<double> fitForwards;
    std::vector<double> curveForwards; // NaN when the input has no dividend curve
    std::vector<double> impliedDividendYields;
    std::vector<QuoteRow> quotes; // one row per input quote, in input order
    std::vector<SliceDiagnostics> slices;
    std::size_t excludedByMoneyness = 0;
    double rmseVolPts = 0.0;
    double maxAbsVolPts = 0.0;
    std::vector<Violation> violations; // empty on success
    ScanResult scan;
};

// theta(psi) = W - rho k psi - c psi^2, c = (1 - rho^2) k^2 / (4 W): the slice variance for which
// the slice (theta, rho, psi) has total variance W at log-moneyness k (see step 4 above).
double anchorTheta(double W, double rho, double k, double psi);

// The input checks calibrate() starts with (400 on failure); calibrateCached runs them before it
// touches the curves for the cache key, so both paths reject a bad input identically.
void validateInput(const CalibrationInput& input);

CalibrationResult calibrate(const CalibrationInput& input);

// The scan the calibrator runs after a fit, reused to validate user-supplied slices: the slice
// times, their midpoints, 0.5 T_1 and 1.5 T_N, k in [-1.5, 1.0], 201 points.
ScanResult defaultScan(const Surface& surface);

}} // namespace quantra::ssvi

#endif // QUANTRA_SSVI_CALIBRATOR_H
