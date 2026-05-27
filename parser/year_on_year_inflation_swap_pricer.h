#ifndef QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_PRICER_H
#define QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_PRICER_H

/**
 * YearOnYearInflationSwapPricer — pure QuantLib pricing core for YYIIS trades.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * year_on_year_inflation_swap_mapper.{h,cpp}.
 */

#include <memory>
#include <string>
#include <vector>

#include <ql/cashflow.hpp>
#include <ql/cashflows/cpicoupon.hpp>
#include <ql/instruments/yearonyearinflationswap.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/period.hpp>
#include <ql/time/schedule.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// One YYIIS trade lifted out of the FB request by the mapper. The mapper has
/// already converted enums to QL types and parsed both schedules; the pricer
/// never touches FlatBuffers.
struct YearOnYearInflationSwapTrade {
    QuantLib::YearOnYearInflationSwap::Type swapType =
        QuantLib::YearOnYearInflationSwap::Payer;
    double notional = 0.0;
    QuantLib::Schedule fixedSchedule;
    double fixedRate = 0.0;
    QuantLib::DayCounter fixedDayCounter;
    QuantLib::Schedule yoySchedule;
    std::string inflationIndexId;
    QuantLib::Period observationLag;
    QuantLib::CPI::InterpolationType observationInterpolation = QuantLib::CPI::AsIndex;
    double spread = 0.0;
    QuantLib::DayCounter yoyDayCounter;
    QuantLib::Calendar paymentCalendar;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::ModifiedFollowing;

    std::string discountingCurveId;
    std::string inflationCurveId;
};

struct YearOnYearInflationSwapInputs {
    std::vector<YearOnYearInflationSwapTrade> trades;
    bool includeFlows = false;
};

/**
 * Plain cash-flow representation produced by the pricer and consumed by the
 * mapper. Mirrors the legacy buildSwapLegFlow output verbatim: accrual fields
 * only appear when the cashflow is a Coupon; fixing fields only appear when
 * the cashflow is a FloatingRateCoupon. The pricer pre-computes everything
 * that depends on the discount curve so the mapper never touches QuantLib.
 */
struct YearOnYearInflationSwapFlowPlain {
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
struct YearOnYearInflationSwapPerSwap {
    double npv = 0.0;
    double fairRate = 0.0;
    double fairSpread = 0.0;
    double fixedLegBps = 0.0;
    double yoyLegBps = 0.0;
    double fixedLegNpv = 0.0;
    double yoyLegNpv = 0.0;
    bool includeFlows = false;
    std::vector<YearOnYearInflationSwapFlowPlain> fixedFlows;
    std::vector<YearOnYearInflationSwapFlowPlain> yoyFlows;
};

struct YearOnYearInflationSwapResult {
    std::vector<YearOnYearInflationSwapPerSwap> swaps;
};

class YearOnYearInflationSwapPricer {
public:
    YearOnYearInflationSwapResult price(const YearOnYearInflationSwapInputs& inputs,
                                        const PricingRegistry& reg,
                                        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_PRICER_H
