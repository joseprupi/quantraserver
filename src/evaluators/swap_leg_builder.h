#ifndef QUANTRA_SWAP_LEG_BUILDER_H
#define QUANTRA_SWAP_LEG_BUILDER_H

/**
 * Shared QuantLib leg construction for the swap evaluators (vanilla, OIS,
 * basis and the unified `price-swap`). One place builds a fixed-rate leg, an
 * IBOR leg and an overnight leg from plain leg descriptions, so the products
 * cannot drift apart in schedule / payment-adjustment / pricer details and a
 * single-currency swap priced through `price-swap` reproduces the dedicated
 * endpoint bit-for-bit. The builders return the QuantLib::Leg exactly as
 * QuantLib's own instruments assemble it: FixedRateLeg / IborLeg as
 * QuantLib::VanillaSwap, FixedRateLeg (with payment lag and calendar) /
 * OvernightLeg as QuantLib::OvernightIndexedSwap.
 *
 * FlatBuffers-free by construction (it is included from evaluators, which
 * Suite 0 greps).
 */

#include <ql/cashflow.hpp>
#include <ql/cashflows/rateaveraging.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
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
    /// Calendar the payment dates are adjusted (and lagged) on; empty = the
    /// schedule's calendar (QuantLib's own default).
    QuantLib::Calendar paymentCalendar;
    /// Payment lag in business days after each accrual end (the fixed leg of
    /// an OIS pays with the overnight leg's lag); 0 = paid on the adjusted
    /// accrual end.
    int paymentLag = 0;
};

/// Plain description of a floating coupon leg on an IBOR index: one fixing
/// per period, `fixingDays` before the accrual start (or at its end with
/// `inArrears`).
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

/// Plain description of an overnight coupon leg: the index's daily fixings
/// compounded (or averaged) over each schedule period, with the payment and
/// observation conventions QuantLib::OvernightIndexedSwap exposes. The coupon
/// day counter is the index's own (QuantLib's convention for OIS legs).
struct OvernightLegSpec {
    QuantLib::Schedule schedule;
    /// One entry per coupon period, or a single entry for a constant notional.
    std::vector<double> notionals;
    double spread = 0.0;
    QuantLib::BusinessDayConvention paymentConvention = QuantLib::Following;
    /// Calendar the payment dates are adjusted (and lagged) on; empty = the
    /// schedule's calendar.
    QuantLib::Calendar paymentCalendar;
    /// Payment lag in business days after each accrual end.
    int paymentLag = 0;
    QuantLib::RateAveraging::Type averagingMethod = QuantLib::RateAveraging::Compound;
    /// Fixing lookback in business days; <= 0 = no lookback (QuantLib's
    /// Null<Natural>, never a literal zero-day lookback).
    int lookbackDays = 0;
    /// Lockout (rate-cutoff) days at the period end; 0 = none.
    int lockoutDays = 0;
    bool applyObservationShift = false;
    /// QuantLib's telescopic value-dates optimisation (performance toggle).
    bool telescopicValueDates = false;
};

/// FixedRateLeg(schedule).withNotionals(notionals).withCouponRates(rate, dc)
/// .withPaymentAdjustment(paymentConvention).withPaymentCalendar(calendar)
/// .withPaymentLag(lag) — the chain QuantLib::VanillaSwap (lag 0, schedule
/// calendar) and QuantLib::OvernightIndexedSwap build their fixed leg with.
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

/// OvernightLeg on `index` (already cloned onto its forwarding curve) with
/// the spec's notionals, spread, payment convention / calendar / lag,
/// averaging method, lookback, lockout and observation shift: the chain
/// QuantLib::OvernightIndexedSwap builds its overnight leg with, so the OIS
/// endpoint and `price-swap` share one construction. The coupons take the
/// index's day counter and carry QuantLib's own compounding pricer.
QuantLib::Leg BuildOvernightLeg(
    const OvernightLegSpec& spec, const std::shared_ptr<QuantLib::OvernightIndex>& index);

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
