#ifndef QUANTRA_BASIS_SWAP_PRICER_H
#define QUANTRA_BASIS_SWAP_PRICER_H

/**
 * BasisSwapPricer — pure QuantLib pricing core for basis swaps (two
 * floating IBOR legs on different tenors, one or both carrying a spread).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * basis_swap_mapper.{h,cpp}. The pricer reads market data exclusively from the
 * QL-typed registry fields (reg.rates.curves, reg.rates.indices).
 */

#include <memory>
#include <string>
#include <vector>

#include <ql/instruments/vanillaswap.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/schedule.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

/// Plain floating-leg description (parsed by the mapper). Carries every QL
/// IborLeg knob the legacy parser exposes.
struct BasisSwapFloatingLegData {
    QuantLib::Schedule schedule;
    double notional = 0.0;
    std::string indexId;
    double spread = 0.0;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::Following;
    int fixingDays = 2;
    bool inArrears = false;
};

/// One basis swap trade lifted out of the FB request by the mapper. The swap
/// type follows the VanillaSwap convention: Payer = pay leg1 / receive leg2.
struct BasisSwapTrade {
    QuantLib::VanillaSwap::Type swapType = QuantLib::VanillaSwap::Payer;
    std::string discountingCurveId;
    std::string forwardingCurveLeg1Id;
    std::string forwardingCurveLeg2Id;
    BasisSwapFloatingLegData leg1;
    BasisSwapFloatingLegData leg2;
};

struct BasisSwapInputs {
    std::vector<BasisSwapTrade> trades;
    bool includeFlows = false;
};

/**
 * Plain cash-flow representation produced by the pricer and consumed by the
 * mapper. Mirrors the SwapLegFlow FB table verbatim — the pricer pre-computes
 * discount factor / present value / ISO date strings so the mapper never
 * touches QuantLib types.
 */
struct BasisSwapFlowPlain {
    bool isFloating = false;
    std::string paymentDate;
    std::string accrualStartDate;
    std::string accrualEndDate;
    double amount = 0.0;
    double accrualYearFraction = 0.0;
    double discount = 0.0;
    double presentValue = 0.0;
    double rate = 0.0;
    // Floating-only fields. Left default-constructed for non-floating flows.
    std::string fixingDate;
    double indexFixing = 0.0;
    double spread = 0.0;
};

/// Per-swap result. Mirrors the BasisSwapResponse FB table.
struct BasisSwapPerSwap {
    double npv = 0.0;
    double leg1Npv = 0.0;
    double leg2Npv = 0.0;
    double leg1Bps = 0.0;
    double leg2Bps = 0.0;
    double fairSpreadLeg1 = 0.0;
    double fairSpreadLeg2 = 0.0;
    bool includeFlows = false;
    std::vector<BasisSwapFlowPlain> leg1Flows;
    std::vector<BasisSwapFlowPlain> leg2Flows;
};

struct BasisSwapResult {
    std::vector<BasisSwapPerSwap> swaps;
};

class BasisSwapPricer {
public:
    BasisSwapResult price(const BasisSwapInputs& inputs,
                          const PricingRegistry& reg,
                          const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_BASIS_SWAP_PRICER_H
