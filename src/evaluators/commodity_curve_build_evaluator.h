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

/// One decoded `CommodityCurveBuild`.
struct CommodityCurveBuildSpec {
    /// The contract as decoded (presence and wire form checked, values not).
    CommodityContractData contract;
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
};

struct CommodityCurveBuildInputs {
    /// pricing.as_of_date.
    QuantLib::Date asOf;
    std::vector<CommodityCurveBuildSpec> builds;
    bool includeDetails = false;
};

/// One completed contract, with what the response echoes.
struct BuiltCommodityCurveResult {
    std::string id;
    std::int8_t pricingCalendarWire = 0;
    std::optional<std::vector<std::string>> pricingHolidays;
    std::vector<commodities::ContractMonth> months;
    bool fixingsGiven = false;
    std::vector<commodities::ContractFixing> fixings;
    commodities::CurveBuildReport report;
};

struct CommodityCurveBuildResultSet {
    std::vector<BuiltCommodityCurveResult> curves;
};

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
