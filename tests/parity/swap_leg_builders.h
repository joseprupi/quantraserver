#pragma once

/**
 * FlatBuffers builders for the `SwapLeg` model the swap endpoints share
 * (vanilla, OIS, basis, cross-currency swaps and the swaption underlying),
 * used by the parity suite, the server-client integration test and the
 * example clients. FlatBuffers-only: no QuantLib, no fixture.
 *
 * A `SwapLeg` is one coupon stream in one currency, paid or received by the
 * party whose NPV is reported, discounted on its own curve; floating kinds
 * also name a forwarding curve. The leg is flat: the fields every coupon
 * kind shares (notional, schedule, day count, payment conventions) sit on
 * the leg itself (`SwapLegFields` here), and the `coupons` union holds only
 * what is specific to the kind (FixedCoupons { rate }, IborCoupons { index,
 * spread, ... }, OvernightCoupons { index, averaging, ... }, CmsCoupons).
 * A payer swap is `fixed_leg.pay_receive = Pay` with the other leg
 * `Receive`; the response's `legs[i].npv` is the leg's PV signed from that
 * party's view, `npv` their sum.
 */

#include "basis_swap_generated.h"
#include "ois_swap_generated.h"
#include "swap_leg_generated.h"
#include "vanilla_swap_generated.h"

#include <flatbuffers/flatbuffers.h>

#include <optional>

namespace quantra { namespace testing {

/// The side opposite to `side` (the other leg of a two-leg swap).
inline quantra::enums::PayReceive oppositeSide(quantra::enums::PayReceive side) {
    return side == quantra::enums::PayReceive_Pay ? quantra::enums::PayReceive_Receive
                                                  : quantra::enums::PayReceive_Pay;
}

/// The shared, leg-level fields of a `SwapLeg`: the accrual schedule, the
/// notional (constant, or a per-period vector), the accrual day count and
/// the payment conventions. Every member is optional so a test can leave a
/// presence-required field out and check the 400 naming it; the constructor
/// sets the ones a priced leg always carries. `withPaymentTerms` adds the
/// payment calendar and lag (presence-required on both legs of
/// `price-ois-swap`, optional elsewhere where absent = the schedule's
/// calendar and no lag, rejected with CmsCoupons); `withNotionals` the
/// amortizing vector (vanilla swap legs only).
struct SwapLegFields {
    flatbuffers::Offset<quantra::Schedule> schedule = 0;
    std::optional<double> notional;
    flatbuffers::Offset<flatbuffers::Vector<double>> notionals = 0;
    std::optional<quantra::enums::DayCounter> dayCounter;
    std::optional<quantra::enums::BusinessDayConvention> paymentConvention;
    std::optional<quantra::enums::Calendar> paymentCalendar;
    std::optional<int> paymentLag;

    SwapLegFields() = default;

    /// A constant-notional leg accruing on `schedule` with `dayCounter`,
    /// paying on the `paymentConvention`-adjusted accrual ends.
    SwapLegFields(
        flatbuffers::Offset<quantra::Schedule> schedule_,
        double notional_,
        quantra::enums::DayCounter dayCounter_,
        quantra::enums::BusinessDayConvention paymentConvention_ =
            quantra::enums::BusinessDayConvention_ModifiedFollowing)
        : schedule(schedule_), notional(notional_), dayCounter(dayCounter_),
          paymentConvention(paymentConvention_) {}

    /// Pay `lag` business days on `calendar` after each accrual end.
    SwapLegFields& withPaymentTerms(quantra::enums::Calendar calendar, int lag) {
        paymentCalendar = calendar;
        paymentLag = lag;
        return *this;
    }

    /// Per-period notionals (one per coupon period, schedule order); the
    /// constant `notional` then becomes optional.
    SwapLegFields& withNotionals(flatbuffers::Offset<flatbuffers::Vector<double>> notionals_) {
        notionals = notionals_;
        return *this;
    }
};

/// Build one `SwapLeg` from its shared fields and an already-finished
/// kind-specific coupons table. The coupons kind is taken from the table's
/// type (`SwapCouponsTraits`). `forwardingCurve` is required for the
/// floating kinds and must be left null for FixedCoupons (the engine rejects
/// both mistakes with a 400). The exchange flags are written only when
/// given, so `std::nullopt` exercises "absent = false" on the
/// single-currency endpoints and "presence-required" on the cross-currency
/// one.
template <typename Coupons>
flatbuffers::Offset<quantra::SwapLeg> buildSwapLeg(
    flatbuffers::FlatBufferBuilder& b,
    quantra::enums::PayReceive side,
    const char* currency,
    const SwapLegFields& fields,
    flatbuffers::Offset<Coupons> coupons,
    const char* discountingCurve,
    const char* forwardingCurve = nullptr,
    std::optional<bool> initialExchange = std::nullopt,
    std::optional<bool> finalExchange = std::nullopt) {
    auto ccy = b.CreateString(currency);
    auto disc = b.CreateString(discountingCurve);
    flatbuffers::Offset<flatbuffers::String> fwd = 0;
    if (forwardingCurve) fwd = b.CreateString(forwardingCurve);
    quantra::SwapLegBuilder lb(b);
    lb.add_pay_receive(side);
    lb.add_currency(ccy);
    if (fields.notional) lb.add_notional(*fields.notional);
    if (!fields.notionals.IsNull()) lb.add_notionals(fields.notionals);
    if (!fields.schedule.IsNull()) lb.add_schedule(fields.schedule);
    if (fields.dayCounter) lb.add_day_counter(*fields.dayCounter);
    if (fields.paymentConvention) lb.add_payment_convention(*fields.paymentConvention);
    if (fields.paymentCalendar) lb.add_payment_calendar(*fields.paymentCalendar);
    if (fields.paymentLag) lb.add_payment_lag(*fields.paymentLag);
    lb.add_coupons_type(quantra::SwapCouponsTraits<Coupons>::enum_value);
    lb.add_coupons(coupons.Union());
    lb.add_discounting_curve(disc);
    if (forwardingCurve) lb.add_forwarding_curve(fwd);
    if (initialExchange) lb.add_initial_exchange(*initialExchange);
    if (finalExchange) lb.add_final_exchange(*finalExchange);
    return lb.Finish();
}

/// A single-currency `VanillaSwap`: FixedCoupons on `fixed_leg` (side
/// `fixedSide`), IborCoupons or CmsCoupons on `floating_leg` (the opposite
/// side), each leg with its own shared fields, both in `currency`,
/// discounted on `discountingCurve`, the floating leg projected on
/// `forwardingCurve`. `exchange` sets both exchange flags on both legs when
/// given.
template <typename FloatingCoupons>
flatbuffers::Offset<quantra::VanillaSwap> buildVanillaSwap(
    flatbuffers::FlatBufferBuilder& b,
    quantra::enums::PayReceive fixedSide,
    const char* currency,
    const SwapLegFields& fixedFields,
    flatbuffers::Offset<quantra::FixedCoupons> fixedCoupons,
    const SwapLegFields& floatingFields,
    flatbuffers::Offset<FloatingCoupons> floatingCoupons,
    const char* discountingCurve,
    const char* forwardingCurve,
    std::optional<bool> exchange = std::nullopt) {
    auto fixedLeg = buildSwapLeg(
        b, fixedSide, currency, fixedFields, fixedCoupons, discountingCurve, nullptr, exchange,
        exchange);
    auto floatingLeg = buildSwapLeg(
        b, oppositeSide(fixedSide), currency, floatingFields, floatingCoupons, discountingCurve,
        forwardingCurve, exchange, exchange);
    return quantra::CreateVanillaSwap(b, fixedLeg, floatingLeg);
}

/// A single-currency `OisSwap`: FixedCoupons on `fixed_leg` (side
/// `fixedSide`), OvernightCoupons on `overnight_leg` (the opposite side).
/// Both legs' fields should carry the payment calendar and lag
/// (`withPaymentTerms`): `price-ois-swap` requires them on both legs.
inline flatbuffers::Offset<quantra::OisSwap> buildOisSwap(
    flatbuffers::FlatBufferBuilder& b,
    quantra::enums::PayReceive fixedSide,
    const char* currency,
    const SwapLegFields& fixedFields,
    flatbuffers::Offset<quantra::FixedCoupons> fixedCoupons,
    const SwapLegFields& overnightFields,
    flatbuffers::Offset<quantra::OvernightCoupons> overnightCoupons,
    const char* discountingCurve,
    const char* forwardingCurve) {
    auto fixedLeg =
        buildSwapLeg(b, fixedSide, currency, fixedFields, fixedCoupons, discountingCurve);
    auto overnightLeg = buildSwapLeg(
        b, oppositeSide(fixedSide), currency, overnightFields, overnightCoupons, discountingCurve,
        forwardingCurve);
    return quantra::CreateOisSwap(b, fixedLeg, overnightLeg);
}

/// A single-currency `BasisSwap`: two floating legs (IborCoupons or
/// OvernightCoupons), `leg1` on side `leg1Side`, `leg2` on the opposite one,
/// each with its own shared fields and projected on its own forwarding curve.
template <typename Coupons1, typename Coupons2>
flatbuffers::Offset<quantra::BasisSwap> buildBasisSwap(
    flatbuffers::FlatBufferBuilder& b,
    quantra::enums::PayReceive leg1Side,
    const char* currency,
    const SwapLegFields& leg1Fields,
    flatbuffers::Offset<Coupons1> leg1Coupons,
    const SwapLegFields& leg2Fields,
    flatbuffers::Offset<Coupons2> leg2Coupons,
    const char* discountingCurve,
    const char* forwardingCurveLeg1,
    const char* forwardingCurveLeg2) {
    auto leg1 = buildSwapLeg(
        b, leg1Side, currency, leg1Fields, leg1Coupons, discountingCurve, forwardingCurveLeg1);
    auto leg2 = buildSwapLeg(
        b, oppositeSide(leg1Side), currency, leg2Fields, leg2Coupons, discountingCurve,
        forwardingCurveLeg2);
    return quantra::CreateBasisSwap(b, leg1, leg2);
}

}} // namespace quantra::testing
