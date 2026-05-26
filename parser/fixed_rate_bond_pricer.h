#ifndef QUANTRA_FIXED_RATE_BOND_PRICER_H
#define QUANTRA_FIXED_RATE_BOND_PRICER_H

/**
 * FixedRateBondPricer — pure QuantLib pricing core for fixed-rate bonds.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * fixed_rate_bond_mapper.{h,cpp}.
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ql/instruments/bonds/fixedratebond.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/compounding.hpp>
#include <ql/time/frequency.hpp>

#include "pricing_registry.h"
#include "pricing_context.h"

namespace quantra {

/// One fixed-rate bond trade lifted out of the FB request by the mapper.
struct FixedRateBondTrade {
    std::shared_ptr<QuantLib::FixedRateBond> bond;
    std::string discountingCurveId;
    QuantLib::DayCounter yieldDc;
    QuantLib::Compounding yieldComp = QuantLib::Compounded;
    QuantLib::Frequency yieldFreq = QuantLib::Annual;
};

struct FixedRateBondInputs {
    std::vector<FixedRateBondTrade> trades;
};

/**
 * Plain cash-flow representation produced by the pricer and consumed by the
 * mapper. The pricer pre-computes everything that depends on the discount
 * curve (discount factor, present value, ISO date strings) so the mapper
 * never needs to touch curves or QuantLib date types.
 */
struct FixedRateBondFlowPlain {
    enum class Kind { Interest, PastInterest, Notional };
    Kind kind = Kind::Interest;
    double amount = 0.0;
    double rate = 0.0;          // interest / past-interest only
    double discount = 0.0;      // interest / notional only (zero for past)
    double price = 0.0;         // amount * discount; zero for past
    std::string accrualStartDate; // ISO yyyy-mm-dd, interest / past
    std::string accrualEndDate;   // ISO yyyy-mm-dd, interest / past
    std::string paymentDate;      // ISO yyyy-mm-dd, notional
};

/// Per-bond result. `hasDetails` mirrors PricingRequestOptions.bondPricingDetails.
struct FixedRateBondPerBond {
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
    std::vector<FixedRateBondFlowPlain> flows;
};

struct FixedRateBondResult {
    std::vector<FixedRateBondPerBond> bonds;
};

class FixedRateBondPricer {
public:
    FixedRateBondResult price(const FixedRateBondInputs& inputs,
                              const PricingRegistry& reg,
                              const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_FIXED_RATE_BOND_PRICER_H
