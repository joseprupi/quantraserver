#ifndef QUANTRA_SWAP_LEG_PRICER_H
#define QUANTRA_SWAP_LEG_PRICER_H

/**
 * Valuation of one `SwapLeg` (swap_leg_data.h) against the pricing registry,
 * shared by the vanilla, OIS, basis and cross-currency swap evaluators. For a
 * leg with notional N, coupon leg C (built by swap_leg_builder.h exactly as
 * QuantLib::VanillaSwap / OvernightIndexedSwap build theirs, or a CmsLeg with
 * the requested CMS coupon pricer) and discounting curve D, the leg in its
 * own frame (as the receiver of its coupons sees it) is
 *
 *   L = C  +  [initial_exchange] SimpleCashFlow(-N, schedule start)
 *          +  [final_exchange]   SimpleCashFlow(+N, last coupon payment date)
 *
 * and the pricer reports npvOwn = CashFlows::npv(L, D) and bpsOwn =
 * CashFlows::bps(C, D) (the coupons' basis-point sensitivity, exchanges
 * excluded) as of the evaluation date, flows on that date excluded (as
 * QuantLib's DiscountingSwapEngine does for every swap). The evaluator
 * applies the side (`SwapLegData::sign()`), sums the legs and solves its
 * fair quantity with the leg's signed bps. Market-data checks that need the
 * registry live here: curve and index ids, the index kind against the coupon
 * kind (IborCoupons on an overnight index and OvernightCoupons on an IBOR
 * index are 400s naming the table to use), the leg currency against the
 * index currency, and the CMS vol surface / swap index.
 *
 * FlatBuffers-free by construction (included from evaluators, which Suite 0
 * greps).
 */

#include "pricing_context.h"
#include "pricing_registry.h"
#include "swap_leg_data.h"

#include <ql/cashflow.hpp>
#include <ql/cashflows/couponpricer.hpp>
#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace quantra {

/// Effective CMS pricer settings, reported back as the `used_cms_*` fields
/// of the vanilla swap response (-1 where a setting does not apply to the
/// chosen pricer).
struct CmsPricerUsed {
    quantra::enums::CmsPricerType pricerType = quantra::enums::CmsPricerType_LinearTsr;
    quantra::enums::CmsYieldCurveModel yieldCurveModel =
        quantra::enums::CmsYieldCurveModel_Standard;
    double meanReversion = 0.03;
    double haganLowerLimit = -1.0;
    double haganUpperLimit = -1.0;
    double haganPrecision = -1.0;
    double haganHardUpperLimit = -1.0;
};

/// A CMS coupon pricer built from plain parameters, with what it used.
struct CmsPricerBuildResult {
    std::shared_ptr<QuantLib::FloatingRateCouponPricer> pricer;
    CmsPricerUsed used{};
};

/// Build the CMS coupon pricer (LinearTsr / Hagan analytic / Hagan numeric)
/// on `volEntry` and `discountCurve`, validating the numeric settings (400
/// naming the setting).
CmsPricerBuildResult BuildCmsPricer(
    const CmsPricerParams& params,
    const SwaptionVolEntry& volEntry,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve);

/// A leg assembled for pricing: the coupon leg alone (annuity) and the full
/// leg with the principal exchanges (NPV), both in the leg's own frame.
struct BuiltSwapLeg {
    QuantLib::Leg coupons;
    QuantLib::Leg full;
    const QuantLib::YieldTermStructure* discount = nullptr;
    /// NPV of `full` in the leg's own frame (receiver of the coupons).
    double npvOwn = 0.0;
    /// CashFlows::bps of the coupons in the leg's own frame: the change in
    /// value for a one-basis-point move of the rate / spread, exchanges
    /// excluded.
    double bpsOwn = 0.0;
    /// Set when the leg carries CMS coupons.
    std::optional<CmsPricerUsed> cmsUsed;
};

/// Build and value `leg`. `name` is the wire field the leg came from (e.g.
/// "VanillaSwap.fixed_leg") and prefixes every error message.
BuiltSwapLeg BuildSwapLeg(
    const SwapLegData& leg,
    const std::string& name,
    const PricingRegistry& reg,
    const PricingContext& ctx);

/// Per-leg result of a swap response (mirrors the SwapLegResult wire table).
struct SwapLegPerLeg {
    /// Signed from the reported party's view (sign * npvOwn).
    double npv = 0.0;
    /// `npv` in the swap's reporting currency (== npv on one currency).
    double npvReporting = 0.0;
    std::vector<SwapLegFlowPlain> flows;
};

/// The signed per-leg result of `built` for `leg` in a single-currency swap
/// (npvReporting == npv), with the leg's flows when `includeFlows`.
SwapLegPerLeg SignedLegResult(
    const SwapLegData& leg,
    const BuiltSwapLeg& built,
    const PricingContext& ctx,
    bool includeFlows);

/// Signed basis-point sensitivity of a leg: sign * bpsOwn, the quantity the
/// fair rate / spread formulas divide by.
inline double SignedBps(const SwapLegData& leg, const BuiltSwapLeg& built) {
    return leg.sign() * built.bpsOwn;
}

} // namespace quantra

#endif // QUANTRA_SWAP_LEG_PRICER_H
