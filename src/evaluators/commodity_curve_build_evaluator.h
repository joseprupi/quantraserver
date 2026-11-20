#ifndef QUANTRA_COMMODITY_CURVE_BUILD_EVALUATOR_H
#define QUANTRA_COMMODITY_CURVE_BUILD_EVALUATOR_H

/**
 * CommodityCurveBuildEvaluator — builds futures contracts from their direct
 * prices and average-price quotes with the commodity library
 * (src/commodities, BuildCommodityCurve): one linear system for the months
 * the quotes touch, then the fill of the other live months.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * commodity_curve_build_mapper.{h,cpp}.
 *
 * The registry is built only when an average quote names a discounting
 * curve (CommodityCurveBuildMapper::needsPricingRegistry); the evaluator
 * resolves those ids (404 naming
 * `curves[i].average_quotes[k].discounting_curve`). The valuation date comes
 * decoded in the inputs. The contract is validated by the library's
 * FuturesContract constructor and built by BuildCommodityCurve; every library
 * error gets the `curves[<i>]` prefix.
 */

#include "commodity_curve_build.h"
#include "commodity_parsers.h"
#include "pricing_context.h"
#include "pricing_registry.h"

#include <ql/time/date.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace quantra {

/// One decoded `CommodityCurveBuild`: a contract or a spot price.
struct CommodityCurveBuildSpec {
    /// The contract as decoded (presence and wire form checked, values not).
    /// Exactly one of `contract` and `spot` is set.
    std::optional<CommodityContractData> contract;
    /// The spot price as decoded, point prices optional.
    std::optional<CommoditySpotData> spot;
    /// Spot builds: the wire value of the spot's interpolation, opaque here.
    std::int8_t spotInterpolationWire = 0;
    /// Absent when the request does not give it; required by the library
    /// only when a live month has no price.
    std::optional<commodities::CurveInterpolation> interpolation;
    bool extrapolateFlat = false;
    /// Wire values returned unchanged, opaque here: the pricing calendar's
    /// enum value, the pricing holidays as sent (absent when not sent) and
    /// whether a fixings list was sent.
    std::int8_t pricingCalendarWire = 0;
    std::optional<std::vector<std::string>> pricingHolidays;
    bool fixingsGiven = false;
    /// The average quotes (discount curves not yet resolved) and the
    /// discounting curve id of each (absent when not sent).
    std::vector<commodities::AverageQuote> averageQuotes;
    std::vector<std::optional<std::string>> discountingCurveIds;
    std::optional<commodities::CurveRule> rule;
    /// Contract builds: the basis to another entry (its `base` contract id
    /// in `baseId`; the base contract is resolved by the evaluator).
    std::optional<commodities::BasisInputs> basis;
    std::string baseId;
};

struct CommodityCurveBuildInputs {
    /// pricing.as_of_date.
    QuantLib::Date asOf;
    std::vector<CommodityCurveBuildSpec> builds;
    bool includeDetails = false;
};

/// One completed contract or spot price, with what the response echoes.
struct BuiltCommodityCurveResult {
    /// A spot price (points, spot fields) rather than a contract (months).
    bool isSpot = false;
    std::string id;
    std::int8_t pricingCalendarWire = 0;
    std::optional<std::vector<std::string>> pricingHolidays;
    std::vector<commodities::ContractMonth> months;
    /// Spot: the built points, the interpolation's wire value,
    /// extrapolate_flat and the fixings as sent.
    std::vector<commodities::SpotNode> points;
    std::int8_t spotInterpolationWire = 0;
    bool extrapolateFlat = false;
    std::vector<commodities::SpotFixing> spotFixings;
    bool fixingsGiven = false;
    std::vector<commodities::ContractFixing> fixings;
    commodities::CurveBuildReport report;
};

struct CommodityCurveBuildResultSet {
    std::vector<BuiltCommodityCurveResult> curves;
};

/// One entry of a set of builds, built.
struct BuiltCommodityEntry {
    BuiltCommodityCurveResult row;
    /// The built object (one of the two), as a swap reads it.
    std::shared_ptr<const commodities::FuturesContract> contract;
    std::shared_ptr<const commodities::SpotPrice> spot;
    /// With CommodityCurveSetOptions::jacobians: d output / d input for every
    /// output (contract: live months in contract order; spot: points) and
    /// every input of the entry (its own, `curve` unset) and of its bases
    /// (`curve` = the base entry's index). Own inputs: the build's exact
    /// solve times the fill's derivative; a base's inputs: central
    /// differences of the chain rebuilt with the input moved by
    /// 1e-6 x max(1, |input|).
    commodities::BuildJacobian jacobian;
};

struct CommodityCurveSetOptions {
    /// The list's path in the request (`curves`, `pricing.commodity.curve_builds`).
    std::string listPath = "curves";
    /// Reports with weights and sensitivities (and the bases' inputs in the
    /// sensitivities of a basis entry).
    bool includeDetails = false;
    /// Fill BuiltCommodityEntry::jacobian.
    bool jacobians = false;
    /// Reuse built entries across requests (key = everything a build reads);
    /// `requestKey` holds what the specs do not (the calendar overrides).
    bool cache = false;
    std::string requestKey;
};

/// Build every entry of `builds` (bases first, results in request order).
/// `rates` resolves the average quotes' discounting curves (404 naming
/// `<listPath>[i].average_quotes[k].discounting_curve`). Errors name
/// `<listPath>[i]`.
std::vector<BuiltCommodityEntry> BuildCommodityCurveSet(
    const std::vector<CommodityCurveBuildSpec>& builds,
    const QuantLib::Date& asOf,
    const RatesRegistry& rates,
    const RequestBudget* budget,
    const CommodityCurveSetOptions& options);

/// A refusal of the solve or the fill as the JSON object the error carries
/// as `details` (snake_case fields, enums as strings, numbers to full double
/// precision, only the fields that apply): reason, curve (the index in
/// `curves`), path (`pathPrefix` + the refusal's path), average_quote, month,
/// months, implied_value, quoted_value, gap, depends_on
/// ([{month} | {average_quote}]), rule.
std::string CurveBuildRefusalJson(
    const commodities::CurveBuildRefusal& refusal,
    std::optional<std::size_t> curve,
    const std::string& pathPrefix = "");

class CommodityCurveBuildEvaluator {
public:
    CommodityCurveBuildResultSet evaluate(
        const CommodityCurveBuildInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_COMMODITY_CURVE_BUILD_EVALUATOR_H
