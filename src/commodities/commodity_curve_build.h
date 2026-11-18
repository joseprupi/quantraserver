#ifndef QUANTRA_COMMODITIES_CURVE_BUILD_H
#define QUANTRA_COMMODITIES_CURVE_BUILD_H

/**
 * Completing a futures contract whose months are only partly priced.
 *
 * A month is live when its last trading date is on or after the valuation
 * date: that is exactly when a swap can read the month's price (a pricing
 * day on or after the valuation date reads a month whose last trading date
 * is on or after that day; earlier days read fixings). For every live month
 * without a price:
 *   - the quoted live months (live months with a price) are the nodes, one
 *     per month on its last trading date; the axis is x = calendar days from
 *     the valuation date / 365 (Actual/365 Fixed), and any affine change of
 *     it gives the same prices;
 *   - between the first and the last quote the month's price is the nodes
 *     read on its own last trading date with one of six methods:
 *       Linear        straight line between the two neighbouring quotes;
 *       LogLinear     straight line in log(price) between them (every quoted
 *                     live price must be > 0);
 *       BackwardFlat  the next quote;
 *       ForwardFlat   the previous quote;
 *       KrugerCubic   piecewise cubic through all the quotes whose slope at a
 *                     quote is the harmonic mean of the two neighbouring
 *                     slopes (0 where they change sign), at the first and
 *                     last quote (3 x end slope - next node slope) / 2; with
 *                     two quotes it is the straight line;
 *       NaturalCubic  cubic spline through all the quotes, second derivative
 *                     continuous and 0 at the first and the last quote;
 *     Linear, BackwardFlat and ForwardFlat are the readings of a spot price's
 *     forward points (SpotPrice); the others are the library's
 *     interpolations (LogLinearInterpolation, CubicInterpolation Kruger /
 *     Spline, non-monotonic, second derivative 0 at both ends);
 *   - before the first or after the last quoted live month the build is
 *     refused naming the month, unless `extrapolateFlat`, which takes the
 *     nearest quoted price (never a method continued).
 * Quoted months and expired months (priced or not) are returned unchanged.
 * When every live month is quoted the build is the identity and the
 * interpolation is not read.
 *
 * Messages are relative to the build (`contract.months[3] (2027-04): ...`,
 * `interpolation is required ...`); the caller adds which build.
 */

#include "commodity_futures_contract.h"
#include "commodity_periods.h"
#include "commodity_spot_price.h"
#include "commodity_swap.h"
#include "error.h"

#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace quantra { namespace commodities {

enum class CurveInterpolation {
    Linear,
    LogLinear,
    BackwardFlat,
    ForwardFlat,
    KrugerCubic,
    NaturalCubic
};

/// The method's name as on the wire ("KrugerCubic").
std::string CurveInterpolationName(CurveInterpolation method);

enum class CurveRule { Flat, Smooth };

/// Why the solve or the fill refused a build, with the data a program needs
/// to act on it. Only the fields that apply are set.
struct CurveBuildRefusal {
    enum class Reason {
        QuoteConflict,
        RuleRequired,
        FlatCannotResolve,
        SmoothNotUnique,
        MonthOutsideRange,
        QuoteHasNoLiveMonth,
        NonPositivePrice,
        InterpolationRequired,
        NoPricedMonth,
        NotFinite,
        /// A value of the request a build rule refuses (a quote's periods,
        /// nearby, roll, missing payment field, price): `path` names it.
        InvalidInput
    };
    /// One input of the build: a direct month price or an average quote.
    struct Input {
        std::optional<std::string> month;
        std::optional<std::size_t> averageQuote;
    };
    Reason reason = Reason::QuoteConflict;
    /// The field the refusal is about, relative to the build
    /// (`average_quotes[3]`, `contract.months[5].price`, `rule`); empty
    /// when there is none.
    std::string path;
    std::optional<std::size_t> averageQuote;
    std::optional<std::string> month;
    std::vector<std::string> months;
    std::optional<double> impliedValue;
    std::optional<double> quotedValue;
    std::optional<double> gap;
    std::vector<Input> dependsOn;
    std::optional<CurveRule> rule;
};

/// The reason's name as on the wire ("QuoteConflict").
std::string CurveBuildRefusalReasonName(CurveBuildRefusal::Reason reason);

/// An invalid argument raised by the solve or the fill: the message as
/// always, plus the refusal's data.
class CurveBuildError : public QuantraInvalidArgument {
public:
    CurveBuildError(const std::string& message, CurveBuildRefusal refusal)
        : QuantraInvalidArgument(message), refusal_(std::move(refusal)) {}
    const CurveBuildRefusal& refusal() const { return refusal_; }

private:
    CurveBuildRefusal refusal_;
};

struct FuturesCurveBuildResult {
    /// Every month of the contract in its order, with the filled prices.
    std::vector<ContractMonth> months;
    /// Months whose price the build filled, in month order.
    std::vector<std::string> interpolatedMonths;
    /// Of those, the months outside the quotes (flat extrapolation).
    std::vector<std::string> extrapolatedMonths;
};

/// Complete `contract` as of `asOf` (rules above). `interpolation` is
/// required only when a live month has no price. Invalid argument when:
/// a live month has no price and `interpolation` is absent; no live month
/// is quoted while one must be filled; LogLinear with a quoted live price
/// <= 0; a month to fill lies outside the quoted live months without
/// `extrapolateFlat`; a filled price is not finite. The contract's own rules
/// (months, last trading dates, finite prices) are checked by its
/// constructor.
FuturesCurveBuildResult BuildFuturesCurve(
    const FuturesContract& contract,
    std::optional<CurveInterpolation> interpolation,
    bool extrapolateFlat,
    const QuantLib::Date& asOf);

// ---------------------------------------------------------------------------
// Average-price quotes, solved as one linear system
// ---------------------------------------------------------------------------
//
// An average quote is the fixed price that makes a fixed-for-floating
// average swap on the contract worth zero: leg 1 pays the quote, leg 2
// receives the average of the `nearby` contract month (roll
// `rollDaysBeforeExpiry`) over each period's pricing days. The swap is
// priced by PriceCommoditySwap; its NPV is linear in the month prices, so one
// evaluation gives the equation
//     quote = constant + sum_m weight_m * price_m,
// weight_m = (d NPV / d price_m) / A and constant = (NPV at zero live prices)
// / A, where A = sum over the fixed leg's flows of quantity x discount (the
// fair fixed price's denominator). Fixings and flows already paid fall into
// the constant exactly as the swap treats them.
//
// Unknowns: the live months touched by a quote (a month with a price, which
// is a direct quote, or a month with a non-zero weight in an average quote),
// in month order. Equations: the direct prices in month order, then the
// average quotes in request order, added one at a time (EquationSpace). A
// quote dependent on the earlier ones is Redundant when the value they imply
// equals its quoted value within 1e-8 x max(1, |quote|), otherwise the build
// is refused (conflict). Full rank: Unique. Otherwise Underdetermined and a
// rule picks the solution:
//   Flat    months touched by the same set of quotes share one price;
//   Smooth  among the exact solutions, the one with the least sum of squared
//           second differences p[i-1] - 2 p[i] + p[i+1] over the unknowns in
//           month order (index spacing).
// A month is Solved when the quotes alone determine it (its unit vector is in
// the span of the equations), Assumed when the rule decides it. The months
// are then the nodes of the fill (BuildFuturesCurve) for the live months no
// quote touches.

struct AverageQuote {
    /// The quoted fixed price.
    double price = 0.0;
    std::vector<CommodityPeriod> periods;
    int nearby = 1;
    int rollDaysBeforeExpiry = 0;
    /// Required with more than one period; one period: PerPeriod when absent.
    std::optional<QuantityBasis> quantityBasis;
    /// Required with more than one period (with discountCurve); one period:
    /// paid the calendar day after its last day, discount 1, when absent
    /// (the discount cancels in a one-period quote).
    std::optional<QuantLib::Calendar> paymentCalendar;
    std::optional<int> paymentLag;
    std::optional<QuantLib::BusinessDayConvention> paymentConvention;
    /// Resolved by the caller from `discounting_curve`; empty when not given.
    QuantLib::Handle<QuantLib::YieldTermStructure> discountCurve;
};

enum class CurveStatus { Unique, Underdetermined };
enum class MonthSource { Quoted, Solved, Assumed, Interpolated, Extrapolated };
enum class QuoteStatus { Used, Redundant };

/// d price / d quote: a direct price (`month`) or an average quote (index).
struct QuoteSensitivity {
    std::optional<std::string> month;
    std::optional<std::size_t> averageQuote;
    double value = 0.0;
};

struct MonthReport {
    std::string month;
    double price = 0.0;
    MonthSource source = MonthSource::Quoted;
    /// With details, Quoted / Solved / Assumed months: one entry per input
    /// quote (direct prices in month order, then average quotes).
    std::vector<QuoteSensitivity> sensitivities;
};

struct QuoteWeight {
    std::string month;
    double weight = 0.0;
};

struct AverageQuoteReport {
    /// The quote's fair fixed price on the built contract (PriceCommoditySwap).
    double repriced = 0.0;
    double difference = 0.0;
    QuoteStatus status = QuoteStatus::Used;
    /// With details: quoted = constant + sum weight x price.
    std::vector<QuoteWeight> weights;
    double constant = 0.0;
};

struct CurveBuildReport {
    CurveStatus status = CurveStatus::Unique;
    std::optional<CurveRule> rule;
    int unknowns = 0;
    int equations = 0;
    int rank = 0;
    double maxRepricingDifference = 0.0;
    /// Every live month in contract order.
    std::vector<MonthReport> months;
    std::vector<AverageQuoteReport> averageQuotes;
    /// With details: the largest absolute sensitivity.
    std::optional<double> maxSensitivity;
};

struct CurveBuildInputs {
    std::optional<CurveInterpolation> interpolation;
    bool extrapolateFlat = false;
    std::vector<AverageQuote> averageQuotes;
    std::optional<CurveRule> rule;
    bool includeDetails = false;
};

struct CommodityCurveBuildResult {
    std::vector<ContractMonth> months;
    CurveBuildReport report;
};

/// Build the contract from its direct prices and average quotes (rules
/// above), then fill the untouched live months. Invalid argument naming the
/// field relative to the build (`average_quotes[2]: ...`, `rule is required
/// ...`) for: an invalid quote (periods, nearby, roll, a missing field of a
/// quote with more than one period, a non-finite price), a quote with no live
/// month, a conflict (the implied value, the quoted value, the gap and the
/// quotes involved), an underdetermined system without a rule (the months
/// the quotes do not determine), Flat or Smooth unable to give one exact
/// solution, a result that is not finite, and every error of the fill.
CommodityCurveBuildResult BuildCommodityCurve(
    const FuturesContract& contract, const CurveBuildInputs& inputs, const QuantLib::Date& asOf);

std::string CurveRuleName(CurveRule rule);

/// `build` + (`.` or `: `) + `message`: a message that starts with a field
/// of the build (`contract`, `interpolation`, `extrapolate_flat`,
/// `average_quotes`, `rule`) is joined with a dot, any other with a colon.
/// `build` is e.g. `curves[1]`.
std::string QualifyCurveBuildMessage(const std::string& build, const std::string& message);

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_CURVE_BUILD_H
