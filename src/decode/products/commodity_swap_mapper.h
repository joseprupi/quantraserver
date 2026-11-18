#ifndef QUANTRA_COMMODITY_SWAP_MAPPER_H
#define QUANTRA_COMMODITY_SWAP_MAPPER_H

/**
 * CommoditySwapMapper — the one place commodity swap flatbuffers live.
 * Decodes a PriceCommoditySwapRequest into plain CommoditySwapInputs and
 * serializes the results back into a PriceCommoditySwapResponse, writing
 * exactly what the library returns.
 *
 * Who checks what (every message names the field from the request root,
 * e.g. `swaps[2].commodity_swap.leg1.periods.start_date is required`):
 *   - this mapper: only what the wire alone can tell. Presence of required
 *     tables, strings, enums and ints (pay_receive, periods, quantity,
 *     quantity.basis, the price union member, contract_id / spot_id, nearby,
 *     roll_days_before_expiry, payment_calendar, payment_lag,
 *     payment_convention, discounting_curve); the period form (generated or
 *     explicit, and a generated form's missing piece); dates as YYYY-MM-DD;
 *     a `values` / `prices` list present with no entry (the library cannot
 *     tell it from an absent list); include_observations without
 *     include_flows. Generated periods are built by the library's
 *     GeneratePeriods, which owns the frequency and date-order rules.
 *   - the library (PriceCommoditySwap, called by the evaluator): every value
 *     rule. Period order and overlaps, exactly one of value / values and of
 *     price / prices, per-period counts, finite and >= 0 quantities, finite
 *     prices and spreads, opposite legs, nearby >= 1, roll >= 0, payment lag
 *     >= 0, pricing days, the market data read and results that are not
 *     finite. The evaluator adds the `swaps[<i>].commodity_swap` prefix.
 *   - the evaluator: contract, spot price and curve ids against the
 *     registry (404 naming the field).
 */

#include "commodity_swap_evaluator.h"

#include "commodity_swap_response_generated.h"
#include "price_commodity_swap_request_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

/// A `CommodityPeriods` table (generated or explicit form) as periods;
/// presence and form are checked here, the periods' own rules by the
/// library. Messages name `field` (e.g. `swaps[0].commodity_swap.leg1.periods`).
std::vector<commodities::CommodityPeriod> DecodeCommodityPeriods(
    const quantra::CommodityPeriods* periods, const std::string& field);

class CommoditySwapMapper {
public:
    CommoditySwapInputs toInputs(const quantra::PriceCommoditySwapRequest* req) const;

    flatbuffers::Offset<quantra::PriceCommoditySwapResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const CommoditySwapResultSet& result) const;
};

} // namespace quantra

#endif // QUANTRA_COMMODITY_SWAP_MAPPER_H
