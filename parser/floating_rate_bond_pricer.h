#ifndef QUANTRA_FLOATING_RATE_BOND_PRICER_H
#define QUANTRA_FLOATING_RATE_BOND_PRICER_H

/**
 * FloatingRateBondPricer — pure QuantLib pricing core for floating-rate bonds.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * floating_rate_bond_mapper.{h,cpp}. The pricer reads market data exclusively
 * from the plain-domain registry fields (reg.rates.curves, reg.rates.indices,
 * reg.rates.couponPricerDomains).
 */

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <ql/compounding.hpp>
#include <ql/instruments/bonds/floatingratebond.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/frequency.hpp>
#include <ql/time/schedule.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/**
 * One floating-rate bond trade, in plain form. The mapper parses the FB bond
 * table into these QL-typed fields; the pricer builds the QL FloatingRateBond
 * itself because the forwarding curve and the IborIndex live in the registry,
 * which is constructed after the mapper has run.
 */
struct FloatingRateBondTrade {
    // Bond construction inputs.
    int settlement_days = 0;
    double face_amount = 0.0;
    QuantLib::Schedule schedule;
    QuantLib::DayCounter accrual_day_counter;
    QuantLib::BusinessDayConvention payment_convention = QuantLib::ModifiedFollowing;
    int fixing_days = 0;
    double spread = 0.0;
    bool in_arrears = false;
    double redemption = 0.0;
    QuantLib::Date issue_date;

    // Registry lookups.
    std::string indexId;
    std::string discountingCurveId;
    std::string forwardingCurveId;
    std::string couponPricerId;

    // Yield-quote conventions used for the analytics block (details).
    QuantLib::DayCounter yieldDc;
    QuantLib::Compounding yieldComp = QuantLib::Compounded;
    QuantLib::Frequency yieldFreq = QuantLib::Annual;
};

struct FloatingRateBondInputs {
    std::vector<FloatingRateBondTrade> trades;
};

/**
 * Plain cash-flow representation produced by the pricer and consumed by the
 * mapper. Mirrors the FlowsWrapper union shape the legacy buildFloatingBondFlows
 * emits. `indexFixing` carries the QuantLib FloatingRateCoupon::indexFixing()
 * value as a double; the mapper passes it through the same FB `fixing_date`
 * field the legacy serializer used (the legacy path stores the raw double
 * value via the generated `add_fixing_date` setter, which we preserve
 * byte-identically here).
 */
struct FloatingRateBondFlowPlain {
    enum class Kind { Interest, PastInterest, Notional };
    Kind kind = Kind::Interest;
    double amount = 0.0;
    double rate = 0.0;
    double indexFixing = 0.0;
    double discount = 0.0;
    double price = 0.0;
    std::string accrualStartDate;
    std::string accrualEndDate;
    std::string paymentDate;
};

/// Per-bond result. `hasDetails` mirrors PricingRequestOptions.bondPricingDetails.
struct FloatingRateBondPerBond {
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
    double accruedDays = 0.0;
    std::vector<FloatingRateBondFlowPlain> flows;
};

struct FloatingRateBondResult {
    std::vector<FloatingRateBondPerBond> bonds;
};

class FloatingRateBondPricer {
public:
    FloatingRateBondResult price(const FloatingRateBondInputs& inputs,
                                 const PricingRegistry& reg,
                                 const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_FLOATING_RATE_BOND_PRICER_H
