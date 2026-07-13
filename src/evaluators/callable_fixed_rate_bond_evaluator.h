#ifndef QUANTRA_CALLABLE_FIXED_RATE_BOND_EVALUATOR_H
#define QUANTRA_CALLABLE_FIXED_RATE_BOND_EVALUATOR_H

/**
 * CallableFixedRateBondEvaluator — pure QuantLib pricing core for callable
 * (and puttable) fixed-rate bonds.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * callable_fixed_rate_bond_mapper.{h,cpp}; the bond instrument itself is built
 * by callable_fixed_rate_bond_parser (FB side) and reaches the evaluator as a
 * plain QuantLib::CallableFixedRateBond.
 *
 * The short-rate model is resolved the same way the swaption product resolves
 * it: by id from reg.volatility.modelDomains, as a SwaptionModelDomain of
 * model_type HullWhiteLattice. param_mode=Explicit uses the model's
 * hw_a/hw_sigma; param_mode=Calibrate runs (and cross-request caches) the same
 * Hull-White swaption-vol calibration. The bond is then priced on a
 * TreeCallableFixedRateBondEngine over the discount curve with the request's
 * clamped tree_steps.
 */

#include <memory>
#include <string>
#include <vector>

#include <ql/experimental/callablebonds/callablebond.hpp>

#include "pricing_registry.h"
#include "pricing_context.h"

namespace quantra {

/// Lattice step bounds for the TreeCallableFixedRateBondEngine. tree_steps is
/// optional on the wire: absent => kDefaultTreeSteps; present => clamped into
/// [kMinTreeSteps, kMaxTreeSteps] by the mapper (a clamp is not an error).
inline constexpr int kDefaultTreeSteps = 400;
inline constexpr int kMinTreeSteps = 1;
inline constexpr int kMaxTreeSteps = 2000;

/// One callable-bond trade lifted out of the FB request by the mapper. The
/// bond is a fully constructed QuantLib::CallableFixedRateBond; the model is
/// resolved by id inside the evaluator against the plain-domain registry.
struct CallableFixedRateBondTrade {
    std::shared_ptr<QuantLib::CallableFixedRateBond> bond;
    std::string discountingCurveId;
    std::string modelId;
    /// Already clamped to [kMinTreeSteps, kMaxTreeSteps] by the mapper.
    int treeSteps = kDefaultTreeSteps;
};

struct CallableFixedRateBondInputs {
    std::vector<CallableFixedRateBondTrade> trades;
};

/// Per-bond result. Mirrors CallableFixedRateBondResponse exactly.
struct CallableFixedRateBondPerBond {
    double npv = 0.0;
    double cleanPrice = 0.0;
    double dirtyPrice = 0.0;
    std::string settlementDate; // ISO yyyy-mm-dd
};

struct CallableFixedRateBondResult {
    std::vector<CallableFixedRateBondPerBond> bonds;
};

class CallableFixedRateBondEvaluator {
public:
    CallableFixedRateBondResult evaluate(const CallableFixedRateBondInputs& inputs,
                                         const PricingRegistry& reg,
                                         const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CALLABLE_FIXED_RATE_BOND_EVALUATOR_H
