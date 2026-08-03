#ifndef QUANTRA_OIS_SWAP_EVALUATOR_H
#define QUANTRA_OIS_SWAP_EVALUATOR_H

/**
 * OisSwapEvaluator — pure QuantLib pricing core for overnight-indexed swaps
 * (fixed leg vs compounded/averaged overnight leg).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * ois_swap_mapper.{h,cpp}. The evaluator reads market data exclusively from the
 * QL-typed registry fields (reg.rates.curves, reg.rates.indices).
 */

#include <memory>
#include <string>
#include <vector>

#include <ql/instruments/overnightindexedswap.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/schedule.hpp>
#include <ql/cashflows/rateaveraging.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// Plain fixed-leg description (parsed by the mapper).
struct OisSwapFixedLegData {
    QuantLib::Schedule schedule;
    double notional = 0.0;
    double rate = 0.0;
    QuantLib::DayCounter dayCounter;
};

/// Plain overnight-leg description (parsed by the mapper). Carries every
/// QL OvernightIndexedSwap knob the legacy parser exposes: payment lag /
/// calendar / convention, averaging method, lookback/lockout days, the
/// observation-shift toggle, and the telescopic-value-dates optimization.
struct OisSwapOvernightLegData {
    QuantLib::Schedule schedule;
    double notional = 0.0;
    std::string indexId;
    double spread = 0.0;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::Following;
    QuantLib::Calendar paymentCalendar;
    int paymentLag = 0;
    QuantLib::RateAveraging::Type averagingMethod = QuantLib::RateAveraging::Compound;
    /// 0 = no lookback: the evaluator translates <=0 to QL's Null<Natural>()
    /// (QuantLib's off-state), never to a literal zero-day lookback.
    int lookbackDays = 0;
    int lockoutDays = 0;
    bool applyObservationShift = false;
    bool telescopicValueDates = false;
};

/// One OIS swap trade lifted out of the FB request by the mapper.
struct OisSwapTrade {
    QuantLib::OvernightIndexedSwap::Type swapType = QuantLib::OvernightIndexedSwap::Payer;
    std::string discountingCurveId;
    std::string forwardingCurveId;
    OisSwapFixedLegData fixed;
    OisSwapOvernightLegData overnight;
};

struct OisSwapInputs {
    std::vector<OisSwapTrade> trades;
    bool includeFlows = false;
};

/**
 * Plain cash-flow representation produced by the evaluator and consumed by the
 * mapper. Mirrors the SwapLegFlow FB table verbatim — the evaluator pre-computes
 * discount factor / present value / ISO date strings so the mapper never
 * touches QuantLib types.
 */
struct OisSwapFlowPlain {
    bool isFloating = false;
    std::string paymentDate;
    std::string accrualStartDate;
    std::string accrualEndDate;
    double amount = 0.0;
    double accrualYearFraction = 0.0;
    double discount = 0.0;
    double presentValue = 0.0;
    double rate = 0.0;
    // Floating-only fields. Left default-constructed for fixed flows.
    std::string fixingDate;
    double indexFixing = 0.0;
    double spread = 0.0;
};

/// Per-swap result. Mirrors the OisSwapResponse FB table.
struct OisSwapPerSwap {
    double npv = 0.0;
    double fairRate = 0.0;
    double fairSpread = 0.0;
    double fixedLegNpv = 0.0;
    double overnightLegNpv = 0.0;
    double fixedLegBps = 0.0;
    double overnightLegBps = 0.0;
    bool includeFlows = false;
    std::vector<OisSwapFlowPlain> fixedFlows;
    std::vector<OisSwapFlowPlain> overnightFlows;
};

struct OisSwapResult {
    std::vector<OisSwapPerSwap> swaps;
};

class OisSwapEvaluator {
public:
    OisSwapResult evaluate(const OisSwapInputs& inputs,
                        const PricingRegistry& reg,
                        const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_OIS_SWAP_EVALUATOR_H
