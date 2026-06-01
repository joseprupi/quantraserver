#ifndef QUANTRA_ZERO_COUPON_INFLATION_SWAP_EVALUATOR_H
#define QUANTRA_ZERO_COUPON_INFLATION_SWAP_EVALUATOR_H

/**
 * ZeroCouponInflationSwapEvaluator — pure QuantLib pricing core for ZCIIS trades.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * zero_coupon_inflation_swap_mapper.{h,cpp}.
 */

#include <memory>
#include <string>
#include <vector>

#include <ql/cashflows/cpicoupon.hpp>
#include <ql/instruments/zerocouponinflationswap.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/period.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// One ZCIIS trade lifted out of the FB request by the mapper. The mapper has
/// already converted enums to QL types; the evaluator never touches FlatBuffers.
struct ZeroCouponInflationSwapTrade {
    QuantLib::ZeroCouponInflationSwap::Type swapType =
        QuantLib::ZeroCouponInflationSwap::Payer;
    double notional = 0.0;
    QuantLib::Date startDate;
    QuantLib::Date maturityDate;
    QuantLib::Calendar fixedCalendar;
    QuantLib::BusinessDayConvention fixedConvention = QuantLib::ModifiedFollowing;
    QuantLib::DayCounter dayCounter;
    double fixedRate = 0.0;
    std::string inflationIndexId;
    QuantLib::Period observationLag;
    QuantLib::CPI::InterpolationType observationInterpolation = QuantLib::CPI::AsIndex;
    bool adjustObservationDates = false;
    QuantLib::Calendar inflationCalendar;
    QuantLib::BusinessDayConvention inflationConvention = QuantLib::Following;

    std::string discountingCurveId;
    std::string inflationCurveId;
};

struct ZeroCouponInflationSwapInputs {
    std::vector<ZeroCouponInflationSwapTrade> trades;
    bool includeFlows = false;
};

/**
 * Plain cash-flow representation produced by the evaluator and consumed by the
 * mapper. Mirrors the legacy buildSwapLegFlow output verbatim: accrual fields
 * only appear when the cashflow is a Coupon; fixing fields only appear when
 * the cashflow is a FloatingRateCoupon. The pricer pre-computes everything
 * that depends on the discount curve so the mapper never touches QuantLib.
 */
struct ZeroCouponInflationSwapFlowPlain {
    // Always present.
    std::string paymentDate;
    double amount = 0.0;
    double discount = 0.0;
    double presentValue = 0.0;
    double rate = 0.0;

    // Coupon-only fields (filled when isCoupon == true).
    bool isCoupon = false;
    std::string accrualStartDate;
    std::string accrualEndDate;
    double accrualYearFraction = 0.0;

    // FloatingRateCoupon-only fields (filled when isFloating == true).
    bool isFloating = false;
    std::string fixingDate;
    double indexFixing = 0.0;
    double spread = 0.0;
};

/// Per-swap result. Flow vectors are populated only when includeFlows is true.
struct ZeroCouponInflationSwapPerSwap {
    double npv = 0.0;
    double fairRate = 0.0;
    double fixedLegBps = 0.0;
    double fixedLegNpv = 0.0;
    double inflationLegNpv = 0.0;
    bool includeFlows = false;
    std::vector<ZeroCouponInflationSwapFlowPlain> fixedFlows;
    std::vector<ZeroCouponInflationSwapFlowPlain> inflationFlows;
};

struct ZeroCouponInflationSwapResult {
    std::vector<ZeroCouponInflationSwapPerSwap> swaps;
};

class ZeroCouponInflationSwapEvaluator {
public:
    ZeroCouponInflationSwapResult evaluate(const ZeroCouponInflationSwapInputs& inputs,
                                        const PricingRegistry& reg,
                                        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_ZERO_COUPON_INFLATION_SWAP_EVALUATOR_H
