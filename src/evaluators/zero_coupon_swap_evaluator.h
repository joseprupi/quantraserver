#ifndef QUANTRA_ZERO_COUPON_SWAP_EVALUATOR_H
#define QUANTRA_ZERO_COUPON_SWAP_EVALUATOR_H

/**
 * ZeroCouponSwapEvaluator — pure QuantLib pricing core for zero-coupon swaps
 * (single fixed cashflow vs single compounded-floating cashflow).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * zero_coupon_swap_mapper.{h,cpp}. The evaluator reads market data exclusively
 * from the QL-typed registry (reg.rates.curves, reg.rates.indices).
 */

#include <string>
#include <vector>

#include <ql/instruments/zerocouponswap.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/date.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// One zero-coupon swap trade lifted out of the FB request by the mapper.
struct ZeroCouponSwapTrade {
    QuantLib::ZeroCouponSwap::Type swapType = QuantLib::ZeroCouponSwap::Payer;
    double baseNominal = 0.0;
    QuantLib::Date startDate;
    QuantLib::Date maturityDate;
    /// Discriminator: true => the fixed-rate form (fixedRate + fixedRateDc),
    /// false => the fixed-payment form (fixedPayment).
    bool hasFixedRate = false;
    double fixedPayment = 0.0;
    double fixedRate = 0.0;
    QuantLib::DayCounter fixedRateDc;
    QuantLib::Calendar paymentCalendar;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::Following;
    int paymentDelay = 0;
    std::string indexId;
    std::string discountingCurveId;
    std::string forwardingCurveId;
};

struct ZeroCouponSwapInputs {
    std::vector<ZeroCouponSwapTrade> trades;
};

/// Per-swap result. Mirrors the ZeroCouponSwapResponse FB table.
struct ZeroCouponSwapPerSwap {
    double npv = 0.0;
    double fixedLegNpv = 0.0;
    double floatingLegNpv = 0.0;
    double fairFixedPayment = 0.0;
    /// fair_fixed_rate is only defined when the rate form was used.
    bool hasFairFixedRate = false;
    double fairFixedRate = 0.0;
    double fixedPayment = 0.0;
    std::string startDate;  // ISO yyyy-mm-dd
};

struct ZeroCouponSwapResult {
    std::vector<ZeroCouponSwapPerSwap> swaps;
};

class ZeroCouponSwapEvaluator {
public:
    ZeroCouponSwapResult evaluate(const ZeroCouponSwapInputs& inputs,
                                  const PricingRegistry& reg,
                                  const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_ZERO_COUPON_SWAP_EVALUATOR_H
