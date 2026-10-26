#ifndef QUANTRA_IMPLIED_FORWARDS_EVALUATOR_H
#define QUANTRA_IMPLIED_FORWARDS_EVALUATOR_H

/**
 * ImpliedForwardsEvaluator — pure QuantLib core of `/calibrate-implied-forwards`.
 *
 * Implied forwards, implied funding and implied continuous dividend yields per
 * expiry from a sheet of listed European option quotes, by a weighted
 * least-squares put-call-parity fit per expiry.
 *
 * Put-call parity for European options on the same strike K and expiry T:
 *
 *     C(K) - P(K) = DF * (F - K) = (DF * F) + (-DF) * K
 *
 * i.e. the mid call-minus-put is affine in K with intercept a = DF * F and
 * slope b = -DF. With weights w_i the weighted least-squares line through the
 * points (K_i, y_i = C_i - P_i) is
 *
 *     W    = sum w_i
 *     Kbar = sum w_i K_i / W,   ybar = sum w_i y_i / W
 *     b    = sum w_i (K_i - Kbar) (y_i - ybar) / sum w_i (K_i - Kbar)^2
 *     a    = ybar - b * Kbar
 *
 * and the market-implied quantities are
 *
 *     DF_implied = -b                 (requires b < 0)
 *     F          = a / DF_implied = -a / b   (the strike where C - P = 0)
 *
 * The fit runs twice per expiry: first over every line that survived the
 * quote filters, to obtain a provisional forward F1; then over the lines with
 * K inside [F1 (1 - w), F1 (1 + w)] (w = QuoteFilter.moneyness_window). An
 * expiry with fewer than `min_quotes_per_expiry` pairs after either pass is
 * reported as dropped (not a 400); when no expiry survives the request is a
 * 400 that lists the reasons.
 *
 * Weights: w_i = 1 / ((call_ask_i - call_bid_i) + (put_ask_i - put_bid_i)).
 * If any pair of the expiry has a zero total spread, every pair of that
 * expiry gets weight 1 (equal weights) — a zero spread would otherwise be an
 * infinite weight.
 *
 * Residuals r_i = y_i - (a + b K_i) over the pairs of the final pass:
 * fit_residual_rmse = sqrt(sum r_i^2 / n), max_abs_residual = max |r_i|,
 * both unweighted and in price units.
 *
 * Curve-relative quantities, with T the Act/365F year fraction from the
 * as-of date to the expiry and DF_curve the discounting curve's discount
 * factor at the expiry date:
 *
 *     r(T)              = -ln(DF_curve) / T          (continuous, Act/365F)
 *     q(T)              = r(T) - ln(F / S) / T       (from F = S e^{(r - q) T})
 *     implied_funding   = -ln(DF_implied / DF_curve) / T * 1e4   (bp; > 0 means
 *                         the options imply a higher funding rate than the curve)
 *
 * The returned dividend curve is a list of (date, continuous zero) points:
 * one at the as-of date carrying the first fitted expiry's q (the curve's
 * first node is its reference date, so the short end is flat) and one per
 * fitted expiry with q(T). Discounting on the request curve and dividends on
 * that curve (Act/365F) gives S * DF_div(T) / DF_curve(T) = F exactly at every
 * fitted expiry. Those yields are "equity repo + dividend": whatever the
 * market forward implies, which is what pricing should use.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * implied_forwards_mapper.{h,cpp}.
 */

#include "pricing_context.h"
#include "pricing_registry.h"

#include <ql/handle.hpp>
#include <ql/option.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>

#include <optional>
#include <string>
#include <vector>

namespace quantra {

/// One quote line, already null-guarded by the mapper (absent scalars stay
/// absent; the evaluator reports them as dropped with a reason).
struct OptionQuoteLine {
    QuantLib::Date expiry;
    std::string expiryIso;
    std::optional<double> strike;
    std::optional<double> callBid, callAsk, putBid, putAsk;
    std::optional<double> callOpenInterest, putOpenInterest;
};

/// Quote-quality filter (request values or the schema defaults; echoed back).
struct QuoteFilterData {
    double maxRelativeSpread = 0.05;
    unsigned minQuotesPerExpiry = 4;
    double moneynessWindow = 0.25;
    std::optional<double> minOpenInterest;
};

/// Exercise style of the sheet, as decoded by the mapper.
enum class OptionSheetStyle { European, Bermudan, American };

struct ImpliedForwardsInputs {
    std::string sheetId;
    std::string underlyingId;
    std::vector<OptionQuoteLine> quotes;
    OptionSheetStyle style = OptionSheetStyle::European;
    QuoteFilterData filter;
    std::string discountingCurveId;
    std::string dividendCurveId;
};

struct DroppedQuoteData {
    std::string expiryIso;
    std::optional<double> strike; // absent when the whole expiry was dropped
    std::string reason;
};

struct ImpliedForwardExpiryData {
    std::string expiryIso;
    QuantLib::Date expiry;
    double time = 0.0;
    unsigned quotesUsed = 0;
    std::vector<DroppedQuoteData> dropped;
    bool usable = false;
    std::string droppedReason; // set iff !usable
    std::optional<double> forward;
    std::optional<double> impliedDiscountFactor;
    std::optional<double> curveDiscountFactor;
    std::optional<double> impliedFundingBp;
    std::optional<double> impliedDividendYield;
    std::optional<double> fitResidualRmse;
    std::optional<double> maxAbsResidual;
};

/// One node of the returned dividend curve (continuous zero, Act/365F).
struct DividendCurvePoint {
    std::string dateIso;
    double zeroRate = 0.0;
};

struct ImpliedForwardsResult {
    std::string sheetId;
    std::string underlyingId;
    double spot = 0.0;
    QuoteFilterData filter;
    std::string asOfIso;
    std::vector<ImpliedForwardExpiryData> expiries; // every sheet expiry, date order
    std::string dividendCurveId;
    std::vector<DividendCurvePoint> dividendCurve; // as-of node + fitted expiries
};

/// One quote line that passed the per-line quality rules, with both sides'
/// mids and spreads (shared by the implied-forward fit and the vol-surface
/// calibration, which applies the same rules in the same order).
struct FilteredQuote {
    double strike = 0.0;
    double callBid = 0.0, callAsk = 0.0, putBid = 0.0, putAsk = 0.0;
    double callMid = 0.0, putMid = 0.0;
    double callSpread = 0.0, putSpread = 0.0;
};

/// 400 (QuantraInvalidArgument) unless every threshold is finite and in range.
void validateQuoteFilter(const QuoteFilterData& flt);

/// The per-line rules of one expiry in order: strike > 0, both sides
/// present, finite, non-negative bid / positive ask, not crossed, open
/// interest, relative spread on each side, duplicate strike. Every rejected
/// line is appended to `dropped` with its reason; survivors are returned in
/// sheet order.
std::vector<FilteredQuote> filterExpiryLines(
    const std::vector<const OptionQuoteLine*>& lines,
    const QuoteFilterData& flt,
    std::vector<DroppedQuoteData>& dropped);

/// Quote resolution: an out-of-the-money price below this fraction of the
/// spot (0.24 index points on a 24,000 index) carries no vol information.
constexpr double kQuoteResolution = 1e-5;

/// Black-76 implied vol of `price` on (forward, discountFactor, time):
/// QuantLib's blackFormulaImpliedStdDev (accuracy 1e-12, 1000 iterations)
/// divided by sqrt(time); nullopt when it does not solve (price outside the
/// Black bounds) or is not finite and > 0.
std::optional<double> impliedBlackVol(
    QuantLib::Option::Type type,
    double strike,
    double forward,
    double price,
    double discountFactor,
    double time);

/// One out-of-the-money quote of an expiry with its implied vol: the side
/// `/calibrate-vol-surface` fits and the structured-note vanilla repricing
/// prices (call iff strike >= forward).
struct OutOfTheMoneyQuote {
    double strike = 0.0;
    bool isCall = true;
    double bid = 0.0, ask = 0.0, mid = 0.0;
    double vol = 0.0;
};

/// The out-of-the-money side of every line of one expiry that passes the
/// per-line rules (filterExpiryLines), kept when its mid is above the
/// intrinsic value DF * max(+/-(F - K), 0), at least kQuoteResolution * spot,
/// and has a Black-76 implied vol on (forward, discountFactor, time); every
/// other line is appended to `dropped` with its reason. Survivors are
/// returned sorted by strike.
std::vector<OutOfTheMoneyQuote> selectOutOfTheMoneyQuotes(
    const std::vector<const OptionQuoteLine*>& lines,
    const QuoteFilterData& flt,
    const std::string& expiryIso,
    double spot,
    double forward,
    double discountFactor,
    double time,
    std::vector<DroppedQuoteData>& dropped);

/// Spot and discounting curve of a quote sheet, resolved from the registry
/// (404 when an id is unknown, 400 when the spot is not finite and > 0).
struct SheetMarket {
    double spot = 0.0;
    QuantLib::Handle<QuantLib::YieldTermStructure> curve;
};
SheetMarket resolveSheetMarket(
    const PricingRegistry& reg, const std::string& underlyingId, const std::string& curveId);

class ImpliedForwardsEvaluator {
public:
    ImpliedForwardsResult evaluate(
        const ImpliedForwardsInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_IMPLIED_FORWARDS_EVALUATOR_H
