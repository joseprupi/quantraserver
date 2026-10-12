#ifndef QUANTRA_VANILLA_SWAP_EVALUATOR_H
#define QUANTRA_VANILLA_SWAP_EVALUATOR_H

/**
 * VanillaSwapEvaluator — pure QuantLib pricing core for vanilla swaps: a
 * fixed `SwapLeg` against an IBOR or CMS `SwapLeg` in one currency, each
 * valued on its own discount curve by swap_leg_pricer.h (the builders of
 * swap_leg_builder.h: FixedRateLeg / IborLeg as QuantLib::VanillaSwap
 * assembles them, CmsLeg with the requested pricer).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * vanilla_swap_mapper.{h,cpp}.
 *
 * With s_i the leg signs (-1 Pay / +1 Receive), npvOwn_i and bpsOwn_i the
 * legs' own-frame values (exchanges included in the NPV, excluded from the
 * bps):
 *   legs[i].npv = s_i * npvOwn_i,   npv = legs[0].npv + legs[1].npv
 *   fair_rate   = rate_0   - npv / ((s_0 * bpsOwn_0) / 1e-4)
 *   fair_spread = spread_1 - npv / ((s_1 * bpsOwn_1) / 1e-4)
 * the latter two being QuantLib::VanillaSwap::fairRate's / fairSpread's own
 * formulas (the signed leg BPS divided by the basis point), so the numbers
 * equal the instrument's. Every swap endpoint reports `fair_rate` iff exactly
 * one leg is fixed and `fair_spread` iff a leg is floating (IBOR, overnight
 * or CMS).
 */

#include "pricing_context.h"
#include "pricing_registry.h"
#include "swap_leg_data.h"
#include "swap_leg_pricer.h"

#include <optional>
#include <vector>

namespace quantra {

/// One vanilla swap: the two legs in request order, lifted by the mapper
/// (fixed leg = FixedCoupons, floating leg = IborCoupons or CmsCoupons, one
/// currency, opposite sides — all enforced by the mapper).
struct VanillaSwapTrade {
    SwapLegData fixedLeg;
    SwapLegData floatingLeg;
};

struct VanillaSwapInputs {
    std::vector<VanillaSwapTrade> trades;
    bool includeFlows = false;
};

/// Per-swap result. Mirrors the VanillaSwapResponse wire table.
struct VanillaSwapPerSwap {
    double npv = 0.0;
    /// Absent when the fixed leg's signed bps is zero (nothing to solve for).
    std::optional<double> fairRate;
    /// Spread on the floating leg (IBOR or CMS) that zeroes the NPV; absent
    /// when that leg's signed bps is zero.
    std::optional<double> fairSpread;
    /// fixed_leg then floating_leg.
    std::vector<SwapLegPerLeg> legs;
    /// Set when the floating leg carries CMS coupons (the `used_cms_*` block).
    std::optional<CmsPricerUsed> cmsUsed;
    bool includeFlows = false;
};

struct VanillaSwapResult {
    std::vector<VanillaSwapPerSwap> swaps;
};

class VanillaSwapEvaluator {
public:
    VanillaSwapResult evaluate(
        const VanillaSwapInputs& inputs,
        const PricingRegistry& reg,
        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_VANILLA_SWAP_EVALUATOR_H
