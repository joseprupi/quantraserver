#ifndef QUANTRA_COMMODITY_CURVE_BUILD_MAPPER_H
#define QUANTRA_COMMODITY_CURVE_BUILD_MAPPER_H

/**
 * CommodityCurveBuildMapper — the one place the commodity curve build
 * flatbuffers live. Decodes a BuildCommodityCurvesRequest into plain
 * CommodityCurveBuildInputs and serializes the completed contracts back into
 * a BuildCommodityCurvesResponse, echoing every contract field as sent and
 * writing the filled prices.
 *
 * Who checks what (every message names the field from the request root):
 *   - this mapper: what the wire alone can tell. `pricing.as_of_date` (a
 *     YYYY-MM-DD date); `pricing.commodity` refused (the contracts to
 *     complete are under `curves`, a second copy would be ambiguous);
 *     `curves` present and non-empty; each contract's presence and wire
 *     form through the shared contract decoder
 *     (`curves[1].contract ('SYN_CL').months[2].last_trade_date is
 *     required`); `interpolation`, when present, one of the six
 *     CommodityCurveInterpolation values (a number outside them is a 400
 *     naming the accepted values).
 *   - the library (FuturesContract, BuildFuturesCurve, called by the
 *     evaluator): every value rule. Months and last trading dates
 *     ascending, finite prices, `interpolation` required when a live month
 *     has no price, LogLinear on positive quotes only, a quoted live month
 *     to fill from, a month outside the quoted ones without
 *     `extrapolate_flat`, a filled price not finite.
 *     The evaluator adds the `curves[<i>]` prefix.
 *
 * Registry only when needed: without a quote naming a `discounting_curve`
 * the endpoint reads only `as_of_date` and `calendar_overrides` from
 * `pricing` and needs no curve (needsPricingRegistry); other `pricing`
 * blocks are then not read. Each average quote: `price`, `periods` (the
 * swap leg's period decoder), `nearby` and `roll_days_before_expiry`
 * presence-required; the payment terms, `quantity_basis` and
 * `discounting_curve` optional here (the library requires them for a quote
 * of several periods); `rule` Flat or Smooth.
 */

#include "commodity_curve_build_evaluator.h"

#include "build_commodity_curves_request_generated.h"
#include "build_commodity_curves_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

/// One `CommodityCurveBuild` decoded (presence and wire form); `base` is its
/// path in the request (`curves[2]`, `pricing.commodity.curve_builds[0]`)
/// and `index` its index in that list (the `curve` of the error details).
/// Used by this endpoint and by the swap request's pricing.commodity.
CommodityCurveBuildSpec DecodeCommodityCurveBuild(
    const quantra::CommodityCurveBuild* entry, const std::string& base, std::size_t index);

/// Build `pricing.commodity.curve_builds` (as /build-commodity-curves builds
/// `curves`, with jacobians; through the curve cache when it is enabled) and
/// register each built contract / spot price in `commodity` under its id. An
/// id given twice (among the built entries of one kind, or also in
/// `contracts` / `spot_prices`) is a 400. `requestKey`: what else the builds
/// read (the calendar overrides), for the cache key.
void RegisterCommodityCurveBuilds(
    CommodityRegistry& commodity,
    const RatesRegistry& rates,
    const flatbuffers::Vector<flatbuffers::Offset<quantra::CommodityCurveBuild>>* builds,
    const QuantLib::Date& asOf,
    const RequestBudget& budget,
    const std::string& requestKey);

class CommodityCurveBuildMapper {
public:
    /// The pricing registry (which requires `pricing.rates.curves`) is built
    /// only when an average quote names a discounting curve (see
    /// detail::has_registry_need_hook).
    bool needsPricingRegistry(const CommodityCurveBuildInputs& inputs) const;

    CommodityCurveBuildInputs toInputs(const quantra::BuildCommodityCurvesRequest* req) const;

    flatbuffers::Offset<quantra::BuildCommodityCurvesResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CommodityCurveBuildResultSet& result) const;
};

} // namespace quantra

#endif // QUANTRA_COMMODITY_CURVE_BUILD_MAPPER_H
