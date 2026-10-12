#ifndef QUANTRA_OIS_SWAP_EVALUATOR_H
#define QUANTRA_OIS_SWAP_EVALUATOR_H

/**
 * OisSwapEvaluator — pure QuantLib pricing core for overnight-indexed swaps:
 * a fixed `SwapLeg` against an overnight `SwapLeg` in one currency, each
 * valued on its own discount curve by swap_leg_pricer.h (the builders of
 * swap_leg_builder.h: FixedRateLeg with the fixed leg's own payment calendar
 * and lag, OvernightLeg with the overnight conventions — the chains
 * QuantLib::OvernightIndexedSwap assembles).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * ois_swap_mapper.{h,cpp}.
 *
 * With s_i the leg signs (-1 Pay / +1 Receive), npvOwn_i and bpsOwn_i the
 * legs' own-frame values:
 *   legs[i].npv = s_i * npvOwn_i,   npv = legs[0].npv + legs[1].npv
 *   fair_rate   = rate_0 - npv / ((s_0 * bpsOwn_0) / 1e-4)
 * which is QuantLib::OvernightIndexedSwap::fairRate's own formula.
 */

#include "pricing_context.h"
#include "pricing_registry.h"
#include "swap_leg_data.h"
#include "swap_leg_pricer.h"

#include <optional>
#include <vector>

namespace quantra {

/// One OIS: the two legs in request order, lifted by the mapper (fixed leg =
/// FixedCoupons, overnight leg = OvernightCoupons, one currency, opposite
/// sides — all enforced by the mapper).
struct OisSwapTrade {
    SwapLegData fixedLeg;
    SwapLegData overnightLeg;
};

struct OisSwapInputs {
    std::vector<OisSwapTrade> trades;
    bool includeFlows = false;
};

/// Per-swap result. Mirrors the OisSwapResponse wire table.
struct OisSwapPerSwap {
    double npv = 0.0;
    /// Absent when the fixed leg's signed bps is zero (nothing to solve for).
    std::optional<double> fairRate;
    /// fixed_leg then overnight_leg.
    std::vector<SwapLegPerLeg> legs;
    bool includeFlows = false;
};

struct OisSwapResult {
    std::vector<OisSwapPerSwap> swaps;
};

class OisSwapEvaluator {
public:
    OisSwapResult evaluate(
        const OisSwapInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_OIS_SWAP_EVALUATOR_H
