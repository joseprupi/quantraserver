#include "swap_leg_builder.h"

#include "date_convert.h"

#include <ql/cashflows/cmscoupon.hpp>
#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/couponpricer.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/cashflows/overnightindexedcoupon.hpp>
#include <ql/cashflows/rateaveraging.hpp>
#include <ql/handle.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/termstructures/volatility/optionlet/optionletvolatilitystructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>
#include <ql/utilities/null.hpp>

#include <limits>

namespace quantra {

QuantLib::Leg BuildFixedRateLeg(const FixedRateLegSpec& spec) {
    // An empty payment calendar means the schedule's own, which is what
    // FixedRateLeg falls back to by itself; a zero lag is a plain adjustment.
    // Both are passed explicitly so the OIS fixed leg (lagged, on the payment
    // calendar) and the vanilla one go through the identical chain.
    const QuantLib::Calendar paymentCalendar =
        spec.paymentCalendar.empty() ? spec.schedule.calendar() : spec.paymentCalendar;
    return QuantLib::FixedRateLeg(spec.schedule)
        .withNotionals(spec.notionals)
        .withCouponRates(spec.rate, spec.dayCounter)
        .withPaymentAdjustment(spec.paymentConvention)
        .withPaymentCalendar(paymentCalendar)
        .withPaymentLag(spec.paymentLag);
}

QuantLib::Leg BuildIborLeg(
    const FloatingLegSpec& spec, const std::shared_ptr<QuantLib::IborIndex>& index) {
    // An empty payment calendar means the schedule's own and a zero lag a
    // plain adjustment (IborLeg's defaults); both are passed explicitly so a
    // lagged leg and a plain one go through the identical chain.
    const QuantLib::Calendar paymentCalendar =
        spec.paymentCalendar.empty() ? spec.schedule.calendar() : spec.paymentCalendar;
    QuantLib::IborLeg builder = QuantLib::IborLeg(spec.schedule, index)
                                    .withNotionals(spec.notionals)
                                    .withPaymentDayCounter(spec.dayCounter)
                                    .withPaymentAdjustment(spec.paymentConvention)
                                    .withPaymentCalendar(paymentCalendar)
                                    .withPaymentLag(spec.paymentLag)
                                    .withSpreads(spec.spread)
                                    .inArrears(spec.inArrears);
    // An absent override leaves IborLeg's fixing days unset, so every coupon
    // fixes on the index's own fixing days exactly as QuantLib::VanillaSwap's
    // floating leg does.
    if (spec.fixingDays) {
        builder.withFixingDays(static_cast<QuantLib::Natural>(*spec.fixingDays));
    }
    QuantLib::Leg leg = builder;

    // IBOR coupons need a pricer to compute their forward rate; a zero-vol
    // Black pricer reproduces VanillaSwap's own internal pricing for an
    // in-advance leg (no convexity/timing adjustment is triggered) and, for an
    // in-arrears leg, reads the shifted (end-of-period) fixing with no
    // convexity adjustment.
    auto optionletVol = std::make_shared<QuantLib::ConstantOptionletVolatility>(
        0, index->fixingCalendar(), QuantLib::ModifiedFollowing, 0.0, QuantLib::Actual365Fixed());
    auto pricer = std::make_shared<QuantLib::BlackIborCouponPricer>();
    pricer->setCapletVolatility(
        QuantLib::Handle<QuantLib::OptionletVolatilityStructure>(optionletVol));
    QuantLib::setCouponPricer(leg, pricer);
    return leg;
}

QuantLib::Leg BuildOvernightLeg(
    const OvernightLegSpec& spec, const std::shared_ptr<QuantLib::OvernightIndex>& index) {
    // The chain of QuantLib::OvernightIndexedSwap's constructor: no payment
    // day counter (the coupons take the index's), the payment calendar
    // defaulting to the schedule's, lookback <= 0 translated to QuantLib's
    // off-state Null<Natural> (a literal 0 would force a zero-day lookback
    // even on an index with a non-zero intrinsic fixing delay). OvernightLeg
    // attaches its own compounding pricer to every coupon.
    const QuantLib::Calendar paymentCalendar =
        spec.paymentCalendar.empty() ? spec.schedule.calendar() : spec.paymentCalendar;
    const QuantLib::Natural lookbackDays = spec.lookbackDays <= 0
                                               ? QuantLib::Null<QuantLib::Natural>()
                                               : static_cast<QuantLib::Natural>(spec.lookbackDays);
    return QuantLib::OvernightLeg(spec.schedule, index)
        .withNotionals(spec.notionals)
        .withSpreads(spec.spread)
        .withTelescopicValueDates(spec.telescopicValueDates)
        .withAveragingMethod(spec.averagingMethod)
        .withPaymentLag(spec.paymentLag)
        .withPaymentAdjustment(spec.paymentConvention)
        .withPaymentCalendar(paymentCalendar)
        .withLookbackDays(lookbackDays)
        .withLockoutDays(static_cast<QuantLib::Natural>(spec.lockoutDays))
        .withObservationShift(spec.applyObservationShift);
}

void ExtractSwapLegFlows(
    const QuantLib::Leg& leg,
    const QuantLib::YieldTermStructure& discountCurve,
    const QuantLib::Date& asOf,
    std::vector<SwapLegFlowPlain>& out) {
    for (const auto& cf : leg) {
        if (!cf || cf->hasOccurred(asOf)) {
            continue;
        }
        SwapLegFlowPlain f;
        f.paymentDate = DateToIso(cf->date());
        f.amount = cf->amount();
        f.discount = discountCurve.discount(cf->date());
        f.presentValue = f.amount * f.discount;

        auto coupon = std::dynamic_pointer_cast<QuantLib::Coupon>(cf);
        if (!coupon) {
            f.kind = SwapFlowKindDomain::NotionalExchange;
            out.push_back(std::move(f));
            continue;
        }
        f.kind = SwapFlowKindDomain::Coupon;
        f.accrualStartDate = DateToIso(coupon->accrualStartDate());
        f.accrualEndDate = DateToIso(coupon->accrualEndDate());
        f.accrualYearFraction = coupon->accrualPeriod();
        f.rate = coupon->rate();

        auto frc = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(cf);
        if (frc) {
            f.isFloating = true;
            f.gearing = frc->gearing();
            f.fixingDate = DateToIso(frc->fixingDate());
            f.spread = frc->spread();
            double fixing = std::numeric_limits<double>::quiet_NaN();
            try {
                fixing = frc->indexFixing();
                if (std::dynamic_pointer_cast<QuantLib::CmsCoupon>(frc)) {
                    f.hasCmsSwapRate = true;
                    f.cmsSwapRate = fixing;
                }
            } catch (...) {
            }
            f.indexFixing = fixing;
        }
        out.push_back(std::move(f));
    }
}

} // namespace quantra
