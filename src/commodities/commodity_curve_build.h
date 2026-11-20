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
        InvalidInput,
        /// Spot builds: a date read outside the points while extrapolate_flat
        /// is false (a quote's pricing day: `average_quote` and `date`; a
        /// point to fill: `date` and every such `dates`).
        DateOutsideRange,
        /// Spot builds: points to fill and no determined point.
        NoPricedPoint,
        /// Basis builds: the base curve named by `basis.base` is not in the
        /// request (a 404).
        UnknownBase,
        /// Basis builds: the base is not a contract build, or names more
        /// than one entry.
        BaseNotContract,
        /// Basis builds: the bases form a cycle (`cycle`: the curves in it).
        BasisCycle,
        /// Basis builds: a base price cannot be read (`base_curve`,
        /// `base_month`): the base month is not listed, or expired without
        /// its final fixing, or its month average reads no live or fixed day.
        BaseNotDetermined,
        /// Basis builds: a month with both a price and a basis quote.
        BasisAndPrice,
        /// Basis builds: a basis quote on an expired month.
        BasisOnExpiredMonth,
        /// Spot basis builds: the base is not a spot price build, or names
        /// more than one entry.
        BaseNotSpot,
        /// The swap pricer, on a quote's days: a day before the valuation
        /// date without its fixing (`date`, `month`), a month read without a
        /// price, a day that needs a month after the last listed one, a
        /// period without a pricing day.
        MissingFixing,
        MonthWithoutPrice,
        MonthNotListed,
        NoPricingDay,
        /// A `discounting_curve` not in pricing.rates.curves (a 404).
        UnknownDiscountingCurve
    };
    /// One input of the build: a direct price (a month, or a spot point's
    /// date) or an average quote.
    struct Input {
        std::optional<std::string> month;
        std::optional<std::size_t> averageQuote;
        std::optional<std::string> date;
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
    /// Spot builds: the date (YYYY-MM-DD) or dates the refusal is about.
    std::optional<std::string> date;
    std::vector<std::string> dates;
    /// Basis builds: the base curve's index in the request, the base month,
    /// and for a cycle the curves in it (in order).
    std::optional<std::size_t> baseCurve;
    std::optional<std::string> baseMonth;
    std::vector<std::size_t> cycle;
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
//           second divided differences over the unknowns on their last
//           trading dates (x = days from the valuation date / 365), as for a
//           spot build.
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
    /// Basis builds: `price` is a spread, this curve's average minus the
    /// base's average over the same periods (the difference of the two fair
    /// fixed prices). The base side's fair price is a constant (the base is
    /// built first), so the quote stays one linear equation on this curve.
    bool versusBase = false;
    /// Contract builds with versusBase: the base month averaged and its roll.
    std::optional<int> baseNearby;
    std::optional<int> baseRollDaysBeforeExpiry;
};

enum class CurveStatus { Unique, Underdetermined };
enum class MonthSource { Quoted, Solved, Assumed, Interpolated, Extrapolated, Basis };
enum class QuoteStatus { Used, Redundant };

/// d price / d quote: a direct price (`month`, or `date` for a spot point;
/// a basis quote counts as its month's or point's direct price) or an
/// average quote (index). `curve`: the request index of the entry the input
/// belongs to when it is an input of a base; absent for the entry's own.
struct QuoteSensitivity {
    std::optional<std::string> month;
    std::optional<std::size_t> averageQuote;
    double value = 0.0;
    std::optional<std::string> date;
    std::optional<std::size_t> curve;
};

struct MonthReport {
    std::string month;
    double price = 0.0;
    MonthSource source = MonthSource::Quoted;
    /// Basis builds: the month's base price and price - base price.
    std::optional<double> basePrice;
    std::optional<double> basis;
    /// With details, Quoted / Solved / Assumed months: one entry per input
    /// quote (direct prices in month order, then average quotes).
    std::vector<QuoteSensitivity> sensitivities;
};

struct QuoteWeight {
    /// The month read (futures); empty for a spot point.
    std::string month;
    double weight = 0.0;
    /// The point read (spot, YYYY-MM-DD); empty for a futures month.
    std::string date;
};

/// One point of a built spot price.
struct PointReport {
    QuantLib::Date date;
    double price = 0.0;
    MonthSource source = MonthSource::Quoted;
    /// As MonthReport::sensitivities.
    std::vector<QuoteSensitivity> sensitivities;
    /// Basis builds: the point's base price and price - base price.
    std::optional<double> basePrice;
    std::optional<double> basis;
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
    /// Spot builds: every point in date order (months is then empty).
    std::vector<PointReport> points;
};

// ---------------------------------------------------------------------------
// Basis: a contract priced as another contract (the base) plus a basis
// ---------------------------------------------------------------------------
//
// A basis quote on month M gives M the direct price base(M) + value (source
// Basis) before the solve, so average quotes on the contract still work on
// top. base(M):
//   SameMonth     the base contract's month M - base_month_offset months, read
//                 as the swap pricer reads it: its price when live, its fixing
//                 on its last trading date when expired;
//   MonthAverage  the average over the pricing days of calendar month M (the
//                 base's calendar and holidays) of the base contract read with
//                 base_nearby / base_roll_days_before_expiry, fixings for the
//                 days before the valuation date (PriceCommoditySwap's average).
// The live months no quote touches are filled on the BASIS of the determined
// months (price - base) with `interpolation` / `extrapolate_flat` on the
// contract's last trading dates, then the base is added back. The sensitivity
// of a month to its base price is 1 and is not reported.

enum class BasisBasePrice { SameMonth, MonthAverage };

struct BasisQuote {
    /// Contract builds: a month of the contract, "YYYY-MM".
    std::string month;
    double value = 0.0;
    /// Spot builds: the date of one of the spot's points.
    std::optional<QuantLib::Date> date;
};

struct BasisInputs {
    /// The built base contract (every live month priced) and its index in
    /// the request; set by the caller.
    std::shared_ptr<const FuturesContract> base;
    /// Spot builds: the built base spot price; set by the caller.
    std::shared_ptr<const SpotPrice> baseSpot;
    std::size_t baseCurve = 0;
    BasisBasePrice basePrice = BasisBasePrice::SameMonth;
    int baseMonthOffset = 0;
    std::optional<int> baseNearby;
    std::optional<int> baseRollDaysBeforeExpiry;
    std::vector<BasisQuote> quotes;
};

struct CurveBuildInputs {
    std::optional<CurveInterpolation> interpolation;
    bool extrapolateFlat = false;
    std::vector<AverageQuote> averageQuotes;
    std::optional<CurveRule> rule;
    bool includeDetails = false;
    std::optional<BasisInputs> basis;
    /// Also return the jacobian of every live month (spot: every point) to
    /// every input of the build.
    bool fullJacobian = false;
};

/// d output / d input for every output of a build (contract: the live months
/// in contract order; spot: the points) and every input of the build (direct
/// prices, basis quotes counted as their month's or point's direct price,
/// then the average quotes; labelled in `inputs`, value 0). A determined
/// output's row is the solve's exact sensitivity; a filled output's row is
/// the solve's sensitivities of the nodes times the fill's derivative to its
/// nodes, by central differences on the fill (step 1e-6 x max(1, |node|)),
/// exact to rounding for the piecewise-linear fills.
struct BuildJacobian {
    std::vector<QuoteSensitivity> inputs;
    std::vector<std::vector<double>> rows;
};

struct CommodityCurveBuildResult {
    std::vector<ContractMonth> months;
    CurveBuildReport report;
    /// With CurveBuildInputs::fullJacobian.
    BuildJacobian jacobian;
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

// ---------------------------------------------------------------------------
// Spot prices from dated points and average quotes
// ---------------------------------------------------------------------------
//
// The nodes are the spot's points, at the dates the caller chose (nothing
// places a node). A point with a price is a direct quote. An average quote
// is the fixed price of an average swap on the spot price: each pricing day
// reads the points through the spot's own interpolation (Linear,
// BackwardFlat, ForwardFlat: linear in the point prices) and
// extrapolate_flat, so it is one linear equation on the points; its row comes
// from PriceCommoditySwap as for a contract (one implementation of pricing
// days, fixings and discounting). Every point is a node whatever its date:
// the swap pricer reads a point before the valuation date when a later day
// lies between it and the next point. Unknowns = points with a price or a
// weight, in date order; then the solve of a contract build (labels are the
// dates). Smooth uses second divided differences on x = days from the
// valuation date / 365:
//     (p[i+1] - p[i]) / (x[i+1] - x[i]) - (p[i] - p[i-1]) / (x[i] - x[i-1]),
// divided by x[i+1] - x[i-1]. A point no quote reads and without a price is
// read from the determined points with the spot's own interpolation and
// extrapolate_flat (Interpolated / Extrapolated; outside without
// extrapolate_flat: refused, DateOutsideRange).

struct SpotNode {
    QuantLib::Date date;
    std::optional<double> price;
};

struct SpotCurveSpec {
    std::string id;
    PricingCalendar calendar;
    SpotInterpolation interpolation = SpotInterpolation::Linear;
    bool extrapolateFlat = false;
    std::vector<SpotNode> points;
    std::vector<SpotFixing> fixings;
};

struct SpotCurveBuildResult {
    /// Every point with its price.
    std::vector<SpotNode> points;
    CurveBuildReport report;
    /// With CurveBuildInputs::fullJacobian.
    BuildJacobian jacobian;
};

/// Basis on a spot build: the base is another spot price (built first); a
/// point's base price is the base read on the point's date (its
/// interpolation and extrapolate_flat; outside its points without
/// extrapolation: BaseNotDetermined naming the date). A basis quote names a
/// point by date and gives it base + value (source Basis); the points no
/// quote determines are filled on their basis with the spot's own
/// interpolation, then the base is added back.
///
/// Build a spot price from its priced points and average quotes (rules
/// above). `inputs.interpolation` is not read (the spot's own interpolation
/// is used); quotes' nearby / roll are not read. Invalid argument naming the
/// field relative to the build: the spot's own rules (`spot: commodity spot
/// price ...`, path `spot.points[i]...`), the refusals of a contract build,
/// DateOutsideRange and NoPricedPoint.
SpotCurveBuildResult BuildSpotCurve(
    const SpotCurveSpec& spot, const CurveBuildInputs& inputs, const QuantLib::Date& asOf);

std::string CurveRuleName(CurveRule rule);

/// `build` + (`.` or `: `) + `message`: a message that starts with a field
/// of the build (`contract`, `spot`, `interpolation`, `extrapolate_flat`,
/// `average_quotes`, `rule`) is joined with a dot, any other with a colon.
/// `build` is e.g. `curves[1]`.
std::string QualifyCurveBuildMessage(const std::string& build, const std::string& message);

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_CURVE_BUILD_H
