#include "swap_leg_flow_builder.h"

#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/utilities/dataformatters.hpp>
#include "common.h"

namespace quantra {

flatbuffers::Offset<SwapLegFlow> buildSwapLegFlow(
    const std::shared_ptr<QuantLib::CashFlow>& cf,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    flatbuffers::grpc::MessageBuilder& builder,
    QuantLib::Date asOf) {
    if (!cf || cf->hasOccurred(asOf)) {
        return 0;
    }

    auto coupon = std::dynamic_pointer_cast<QuantLib::Coupon>(cf);
    std::ostringstream osPayment, osStart, osEnd;
    osPayment << DateToIso(cf->date());
    auto paymentDate = builder.CreateString(osPayment.str());
    flatbuffers::Offset<flatbuffers::String> accrualStart = 0;
    flatbuffers::Offset<flatbuffers::String> accrualEnd = 0;
    if (coupon) {
        osStart << DateToIso(coupon->accrualStartDate());
        osEnd << DateToIso(coupon->accrualEndDate());
        accrualStart = builder.CreateString(osStart.str());
        accrualEnd = builder.CreateString(osEnd.str());
    }

    flatbuffers::Offset<flatbuffers::String> fixingDate = 0;
    double indexFixing = 0.0;
    double spread = 0.0;
    double rate = 0.0;
    double accrualYearFraction = 0.0;
    auto frc = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(coupon);
    if (frc) {
        std::ostringstream osFix;
        osFix << DateToIso(frc->fixingDate());
        fixingDate = builder.CreateString(osFix.str());
        indexFixing = frc->indexFixing();
        spread = frc->spread();
    }
    if (coupon) {
        rate = coupon->rate();
        accrualYearFraction = coupon->accrualPeriod();
    }

    const double discount = discountCurve->discount(cf->date());
    const double amount = cf->amount();

    SwapLegFlowBuilder fb(builder);
    fb.add_payment_date(paymentDate);
    if (coupon) {
        fb.add_accrual_start_date(accrualStart);
        fb.add_accrual_end_date(accrualEnd);
        fb.add_accrual_year_fraction(accrualYearFraction);
    }
    fb.add_amount(amount);
    fb.add_discount(discount);
    fb.add_present_value(amount * discount);
    fb.add_rate(rate);
    if (frc) {
        fb.add_fixing_date(fixingDate);
        fb.add_index_fixing(indexFixing);
        fb.add_spread(spread);
    }
    return fb.Finish();
}

} // namespace quantra
