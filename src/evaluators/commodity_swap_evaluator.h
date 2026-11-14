#ifndef QUANTRA_COMMODITY_SWAP_EVALUATOR_H
#define QUANTRA_COMMODITY_SWAP_EVALUATOR_H

/**
 * CommoditySwapEvaluator — commodity swaps priced by the commodity library
 * (src/commodities, PriceCommoditySwap): two legs, one paid and one
 * received, each a fixed price or the average over each period's pricing
 * days of a futures contract or of a spot price, plus a spread.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * commodity_swap_mapper.{h,cpp}. The evaluator reads market data exclusively
 * from the registry: contracts and spot prices from reg.commodity, discount
 * curves from reg.rates.curves; an unknown id is a 404 naming the swap and the
 * field (`swaps[2].commodity_swap.leg1.price.contract_id: commodity contract
 * not found: X`). Every error of the library gets the same
 * `swaps[<i>].commodity_swap` prefix. The valuation date is the request's
 * as-of date (ctx.asOf).
 */

#include "commodity_swap.h"
#include "pricing_context.h"
#include "pricing_registry.h"

#include <array>
#include <string>
#include <vector>

namespace quantra {

/// One decoded leg: the library leg with its market references still as ids.
/// The mapper fills every field of `leg` except the futures contract / spot
/// price pointer of a floating price and the discount curve.
struct CommoditySwapLegSpec {
    commodities::CommoditySwapLeg leg;
    /// Futures floating price: id of a pricing.commodity.contracts entry.
    std::string contractId;
    /// Spot floating price: id of a pricing.commodity.spot_prices entry.
    std::string spotId;
    /// Id of a pricing.rates.curves curve.
    std::string discountingCurveId;
};

struct CommoditySwapTrade {
    std::array<CommoditySwapLegSpec, 2> legs;
};

struct CommoditySwapInputs {
    std::vector<CommoditySwapTrade> trades;
    bool includeFlows = false;
    /// Only with includeFlows (the mapper refuses it alone).
    bool includeObservations = false;
};

struct CommoditySwapResultSet {
    std::vector<commodities::CommoditySwapResult> swaps;
    bool includeFlows = false;
    bool includeObservations = false;
};

class CommoditySwapEvaluator {
public:
    CommoditySwapResultSet evaluate(
        const CommoditySwapInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_COMMODITY_SWAP_EVALUATOR_H
