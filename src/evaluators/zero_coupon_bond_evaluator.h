#ifndef QUANTRA_ZERO_COUPON_BOND_EVALUATOR_H
#define QUANTRA_ZERO_COUPON_BOND_EVALUATOR_H

/**
 * ZeroCouponBondEvaluator — pure QuantLib pricing core for zero-coupon bonds.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * zero_coupon_bond_mapper.{h,cpp}.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ql/instruments/bond.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/compounding.hpp>
#include <ql/time/frequency.hpp>

#include "pricing_registry.h"
#include "pricing_context.h"

namespace quantra {

/// One zero-coupon bond trade lifted out of the FB request by the mapper. The
/// bond is a plain QuantLib::Bond (a ZeroCouponBond); the evaluator only uses
/// Bond methods so it stays wire-format agnostic.
struct ZeroCouponBondTrade {
    std::shared_ptr<QuantLib::Bond> bond;
    std::string discountingCurveId;
    QuantLib::DayCounter yieldDc;
    QuantLib::Compounding yieldComp = QuantLib::Compounded;
    QuantLib::Frequency yieldFreq = QuantLib::Annual;
};

struct ZeroCouponBondInputs {
    std::vector<ZeroCouponBondTrade> trades;
};

/// Per-bond result. `hasDetails` mirrors PricingRequestOptions.bondPricingDetails.
struct ZeroCouponBondPerBond {
    double npv = 0.0;
    bool hasDetails = false;
    double cleanPrice = 0.0;
    double dirtyPrice = 0.0;
    double accruedAmount = 0.0;
    double yield = 0.0;
    double modifiedDuration = 0.0;
    double macaulayDuration = 0.0;
    double convexity = 0.0;
    double bps = 0.0;
    std::uint32_t accruedDays = 0;
    std::string settlementDate;  // ISO yyyy-mm-dd
};

struct ZeroCouponBondResult {
    std::vector<ZeroCouponBondPerBond> bonds;
};

class ZeroCouponBondEvaluator {
public:
    ZeroCouponBondResult evaluate(const ZeroCouponBondInputs& inputs,
                                  const PricingRegistry& reg,
                                  const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_ZERO_COUPON_BOND_EVALUATOR_H
