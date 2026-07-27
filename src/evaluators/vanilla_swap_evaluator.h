#ifndef QUANTRA_VANILLA_SWAP_EVALUATOR_H
#define QUANTRA_VANILLA_SWAP_EVALUATOR_H

/**
 * VanillaSwapEvaluator — pure QuantLib pricing core for vanilla swaps
 * (IBOR floating leg or CMS floating leg, mutually exclusive).
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_evaluator_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * vanilla_swap_mapper.{h,cpp}.
 */

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <ql/cashflow.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/period.hpp>
#include <ql/time/schedule.hpp>

#include "cms_leg_parser.h"      // CmsPricerUsed (plain struct) and enum-only deps
#include "pricing_registry.h"
#include "pricing_context.h"

namespace quantra {

/**
 * Plain CMS pricer params extracted from FB by the mapper. Mirrors the
 * fields cms_leg_parser.makeCouponPricer consumes — but as plain values, so
 * the pricer never sees the FB CmsPricerSpec table directly.
 */
struct VanillaSwapCmsPricerParams {
    quantra::enums::CmsPricerType pricerType =
        quantra::enums::CmsPricerType_LinearTsr;
    quantra::enums::CmsYieldCurveModel yieldCurveModel =
        quantra::enums::CmsYieldCurveModel_Standard;
    double meanReversion = 0.03;
    double haganLowerLimit = 0.0;
    double haganUpperLimit = 1.0;
    double haganPrecision = 1.0e-6;
    /// Negative sentinel preserved: the evaluator translates <=0 to QL's default.
    double haganHardUpperLimit = -1.0;
};

/// Plain fixed-leg description (parsed by the mapper).
struct VanillaSwapFixedLegData {
    QuantLib::Schedule schedule;
    double notional = 0.0;
    /// Optional per-period notionals (one per coupon period). Empty => constant
    /// `notional`; non-empty => amortizing/step-up fixed leg. Validated by the
    /// mapper against the schedule before it reaches the evaluator.
    std::vector<double> notionals;
    double rate = 0.0;
    QuantLib::DayCounter dayCounter;
    /// Used only by the CMS branch (FixedRateLeg builder); the IBOR branch
    /// passes its schedule directly to QuantLib::VanillaSwap which doesn't
    /// re-adjust payments.
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::ModifiedFollowing;
};

/// Plain IBOR floating-leg description (parsed by the mapper).
struct VanillaSwapIborLegData {
    QuantLib::Schedule schedule;
    double notional = 0.0;
    /// Optional per-period notionals (one per coupon period). Empty => constant
    /// `notional`; non-empty => amortizing/step-up floating leg. Validated by
    /// the mapper against the schedule before it reaches the evaluator.
    std::vector<double> notionals;
    std::string indexId;
    double spread = 0.0;
    QuantLib::DayCounter dayCounter;
    /// Per-coupon fixing-days override applied via IborLeg::withFixingDays.
    /// Defaults to the FB schema default (2). Honored on the manual-leg path
    /// (in-arrears or amortizing); the plain in-advance QuantLib::VanillaSwap
    /// path uses the index's own fixing days.
    int fixingDays = 2;
    /// When true the coupon fixes at the end of its accrual period
    /// (IborLeg::inArrears). Forces the manual-leg path since QuantLib::VanillaSwap
    /// cannot express an in-arrears floating leg.
    bool inArrears = false;
};

/// Plain CMS floating-leg description (parsed by the mapper).
struct VanillaSwapCmsLegData {
    QuantLib::Schedule schedule;
    double notional = 0.0;
    std::string swapIndexId;
    QuantLib::Period swapTenor;
    std::string swaptionVolId;
    /// Negative sentinel preserved: evaluator falls back to swap-index spot_days.
    int fixingDays = -1;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::ModifiedFollowing;
    double gear = 1.0;
    double spread = 0.0;
    /// Optional per-coupon cap/floor strikes. Present => a CappedFlooredCmsCoupon
    /// leg (capped/floored at the given rate). Absent => plain CMS coupons.
    std::optional<double> cap;
    std::optional<double> floor;
    VanillaSwapCmsPricerParams pricerParams{};
};

/// One swap trade. `branch` selects ibor vs cms fields.
struct VanillaSwapTrade {
    enum class Branch { Ibor, Cms };
    Branch branch = Branch::Ibor;
    QuantLib::VanillaSwap::Type swapType = QuantLib::VanillaSwap::Payer;
    std::string discountingCurveId;
    std::string forwardingCurveId;
    VanillaSwapFixedLegData fixed;
    VanillaSwapIborLegData ibor;   // populated when branch == Ibor
    VanillaSwapCmsLegData cms;     // populated when branch == Cms
};

struct VanillaSwapInputs {
    std::vector<VanillaSwapTrade> trades;
    bool includeFlows = false;
};

/**
 * Plain cash-flow representation produced by the evaluator and consumed by the
 * mapper. Mirrors the SwapLegFlow FB table verbatim — the evaluator pre-computes
 * everything that depends on the discount curve (discount, present_value,
 * ISO date strings) so the mapper never touches QuantLib types.
 */
struct VanillaSwapFlowPlain {
    bool isFloating = false;
    std::string paymentDate;
    std::string accrualStartDate;
    std::string accrualEndDate;
    double amount = 0.0;
    double accrualYearFraction = 0.0;
    double gearing = 1.0;
    double discount = 0.0;
    double presentValue = 0.0;
    double rate = 0.0;
    // Floating-only fields. Left default-constructed for fixed flows.
    std::string fixingDate;
    double indexFixing = 0.0;
    bool hasCmsSwapRate = false;
    double cmsSwapRate = 0.0;
    double spread = 0.0;
};

/// Per-swap result. `hasCmsLeg` toggles the optional CMS diagnostics block
/// the mapper emits.
struct VanillaSwapPerSwap {
    double npv = 0.0;
    double fairRate = 0.0;
    double fairSpread = 0.0;
    double fixedLegNpv = 0.0;
    double floatingLegNpv = 0.0;
    double fixedLegBps = 0.0;
    double floatingLegBps = 0.0;
    bool hasCmsLeg = false;
    CmsPricerUsed cmsUsed{};
    bool includeFlows = false;
    std::vector<VanillaSwapFlowPlain> fixedFlows;
    std::vector<VanillaSwapFlowPlain> floatingFlows;
};

struct VanillaSwapResult {
    std::vector<VanillaSwapPerSwap> swaps;
};

class VanillaSwapEvaluator {
public:
    VanillaSwapResult evaluate(const VanillaSwapInputs& inputs,
                            const PricingRegistry& reg,
                            const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_VANILLA_SWAP_EVALUATOR_H
