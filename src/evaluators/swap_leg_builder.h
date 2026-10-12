#ifndef QUANTRA_SWAP_LEG_BUILDER_H
#define QUANTRA_SWAP_LEG_BUILDER_H

/**
 * Shared QuantLib leg construction for the swap evaluators (vanilla, basis,
 * cross-currency). One place builds a fixed-rate leg, an IBOR leg and a
 * compounded overnight leg from plain leg descriptions, so the three
 * products cannot drift apart in schedule / payment-adjustment / pricer
 * details. The builders return the QuantLib::Leg exactly as the vanilla
 * swap's manual-leg path and the basis swap assembled it before they were
 * factored out here.
 *
 * FlatBuffers-free by construction (it is included from evaluators, which
 * Suite 0 greps).
 */

#include <ql/cashflow.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/date.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/schedule.hpp>

#include <memory>
#include <string>
#include <vector>

namespace quantra {

/// Plain description of a fixed-rate coupon leg.
struct FixedRateLegSpec {
    QuantLib::Schedule schedule;
    /// One entry per coupon period, or a single entry for a constant notional.
    std::vector<double> notionals;
    double rate = 0.0;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::ModifiedFollowing;
};

/// Plain description of a floating coupon leg on an IBOR or overnight index.
/// `fixingDays` / `inArrears` apply to IBOR coupons only: a compounded
/// overnight coupon fixes every day of its period on the index's own fixing
/// lag.
struct FloatingLegSpec {
    QuantLib::Schedule schedule;
    /// One entry per coupon period, or a single entry for a constant notional.
    std::vector<double> notionals;
    double spread = 0.0;
    QuantLib::DayCounter dayCounter;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::ModifiedFollowing;
    int fixingDays = 2;
    bool inArrears = false;
};

/// FixedRateLeg(schedule).withNotionals(notionals).withCouponRates(rate, dc)
/// .withPaymentAdjustment(paymentConvention).
QuantLib::Leg BuildFixedRateLeg(const FixedRateLegSpec& spec);

/// IborLeg on `index` (already cloned onto its forwarding curve) with the
/// spec's notionals, payment day counter / adjustment, spread, fixing days
/// and in-arrears flag. The coupons carry a Black pricer with a zero
/// constant optionlet volatility: in advance it reproduces the plain forward
/// (QuantLib::VanillaSwap's own pricing bit-for-bit); in arrears it reads the
/// end-of-period fixing with no convexity adjustment (none is priced — no
/// caplet volatility is supplied).
QuantLib::Leg BuildIborLeg(
    const FloatingLegSpec& spec, const std::shared_ptr<QuantLib::IborIndex>& index);

/// OvernightLeg on `index` (already cloned onto its forwarding curve):
/// daily fixings compounded over each schedule period (RateAveraging::Compound),
/// paid at the period end adjusted with `paymentConvention`, no payment lag,
/// no lookback / lockout, spread added to the compounded rate. `fixingDays`
/// and `inArrears` of the spec are not read.
QuantLib::Leg BuildOvernightLeg(
    const FloatingLegSpec& spec, const std::shared_ptr<QuantLib::OvernightIndex>& index);

/// Kind of a leg cash flow (mirrors the wire enum SwapFlowKind; value-locked
/// in the mapper's conversion).
enum class SwapFlowKindDomain : int {
    Coupon = 0,
    NotionalExchange = 1,
};

/**
 * Plain cash-flow record for a leg that may mix coupons and principal flows.
 * Mirrors the SwapLegFlow wire table: the evaluator pre-computes everything
 * that depends on the discount curve (discount, present value, ISO dates) so
 * the mapper never touches QuantLib types. For a NotionalExchange only
 * paymentDate / amount / discount / presentValue / kind are meaningful.
 */
struct SwapLegFlowPlain {
    SwapFlowKindDomain kind = SwapFlowKindDomain::Coupon;
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
    // Floating-only fields; default-constructed otherwise.
    std::string fixingDate;
    double indexFixing = 0.0;
    double spread = 0.0;
};

/**
 * Pre-serialize every flow of `leg` that has not occurred at `asOf`, in leg
 * order: fixed and floating coupons as Coupon records (with their accrual,
 * rate, and for floating coupons fixing date / index fixing / spread; a
 * fixing that cannot be read is NaN, which the mapper omits), any other cash
 * flow (the SimpleCashFlow notional exchanges) as a NotionalExchange record.
 */
void ExtractSwapLegFlows(
    const QuantLib::Leg& leg,
    const QuantLib::YieldTermStructure& discountCurve,
    const QuantLib::Date& asOf,
    std::vector<SwapLegFlowPlain>& out);

} // namespace quantra

#endif // QUANTRA_SWAP_LEG_BUILDER_H
