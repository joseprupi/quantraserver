#include "swap_leg_builder.h"

#include "date_convert.h"

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

#include <limits>

namespace quantra {

QuantLib::Leg BuildFixedRateLeg(const FixedRateLegSpec& spec) {
    return QuantLib::FixedRateLeg(spec.schedule)
        .withNotionals(spec.notionals)
        .withCouponRates(spec.rate, spec.dayCounter)
        .withPaymentAdjustment(spec.paymentConvention);
}

QuantLib::Leg BuildIborLeg(
    const FloatingLegSpec& spec, const std::shared_ptr<QuantLib::IborIndex>& index) {
    QuantLib::Leg leg = QuantLib::IborLeg(spec.schedule, index)
                            .withNotionals(spec.notionals)
                            .withPaymentDayCounter(spec.dayCounter)
                            .withPaymentAdjustment(spec.paymentConvention)
                            .withSpreads(spec.spread)
                            .withFixingDays(static_cast<QuantLib::Natural>(spec.fixingDays))
                            .inArrears(spec.inArrears);

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
    const FloatingLegSpec& spec, const std::shared_ptr<QuantLib::OvernightIndex>& index) {
    // OvernightLeg attaches its own compounding pricer to every coupon.
    return QuantLib::OvernightLeg(spec.schedule, index)
        .withNotionals(spec.notionals)
        .withPaymentDayCounter(spec.dayCounter)
        .withPaymentAdjustment(spec.paymentConvention)
        .withSpreads(spec.spread)
        .withAveragingMethod(QuantLib::RateAveraging::Compound);
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
            } catch (...) {
            }
            f.indexFixing = fixing;
        }
        out.push_back(std::move(f));
    }
}

} // namespace quantra
