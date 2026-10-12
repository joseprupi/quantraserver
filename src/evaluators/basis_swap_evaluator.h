#ifndef QUANTRA_BASIS_SWAP_EVALUATOR_H
#define QUANTRA_BASIS_SWAP_EVALUATOR_H

/**
 * BasisSwapEvaluator — pure QuantLib pricing core for basis swaps: two
 * floating `SwapLeg`s (IBOR or overnight coupons) in one currency, each
 * valued on its own discount curve by swap_leg_pricer.h (IborLeg with the
 * zero-volatility Black pricer / OvernightLeg from swap_leg_builder.h).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * basis_swap_mapper.{h,cpp}.
 *
 * With s_i the leg signs (-1 Pay / +1 Receive), npvOwn_i and bpsOwn_i the
 * legs' own-frame values:
 *   legs[i].npv = s_i * npvOwn_i,   npv = legs[0].npv + legs[1].npv
 *   fair_spread = spread_k - npv * 1e-4 / (s_k * bpsOwn_k)
 * for the leg k that carries the request's spread (leg1 when only leg1's
 * spread is non-zero, leg2 otherwise).
 */

#include "pricing_context.h"
#include "pricing_registry.h"
#include "swap_leg_data.h"
#include "swap_leg_pricer.h"

#include <optional>
#include <vector>

namespace quantra {

/// One basis swap: the two legs in request order, lifted by the mapper (each
/// IborCoupons or OvernightCoupons, one currency, opposite sides — all
/// enforced by the mapper).
struct BasisSwapTrade {
    SwapLegData leg1;
    SwapLegData leg2;
};

struct BasisSwapInputs {
    std::vector<BasisSwapTrade> trades;
    bool includeFlows = false;
};

/// Per-swap result. Mirrors the BasisSwapResponse wire table.
struct BasisSwapPerSwap {
    double npv = 0.0;
    /// Par spread of the leg carrying the request's spread; absent when that
    /// leg's signed bps is zero.
    std::optional<double> fairSpread;
    /// leg1 then leg2.
    std::vector<SwapLegPerLeg> legs;
    bool includeFlows = false;
};

struct BasisSwapResult {
    std::vector<BasisSwapPerSwap> swaps;
};

class BasisSwapEvaluator {
public:
    BasisSwapResult evaluate(
        const BasisSwapInputs& inputs, const PricingRegistry& reg, const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_BASIS_SWAP_EVALUATOR_H
