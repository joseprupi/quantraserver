#include "swap_leg_flow_builder.h"

#include <ql/cashflows/coupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/utilities/dataformatters.hpp>

namespace quantra {

flatbuffers::Offset<SwapLegFlow> buildSwapLegFlow(
    const std::shared_ptr<QuantLib::CashFlow>& cf,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    flatbuffers::grpc::MessageBuilder& builder,
    QuantLib::Date asOf) {
    auto coupon = std::dynamic_pointer_cast<QuantLib::Coupon>(cf);
    if (!coupon || coupon->hasOccurred(asOf)) {
        return 0;
    }

    std::ostringstream osPayment, osStart, osEnd;
    osPayment << QuantLib::io::iso_date(coupon->date());
    osStart << QuantLib::io::iso_date(coupon->accrualStartDate());
    osEnd << QuantLib::io::iso_date(coupon->accrualEndDate());
    auto paymentDate = builder.CreateString(osPayment.str());
    auto accrualStart = builder.CreateString(osStart.str());
    auto accrualEnd = builder.CreateString(osEnd.str());

    flatbuffers::Offset<flatbuffers::String> fixingDate = 0;
    double indexFixing = 0.0;
    double spread = 0.0;
    auto frc = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(coupon);
    if (frc) {
        std::ostringstream osFix;
        osFix << QuantLib::io::iso_date(frc->fixingDate());
        fixingDate = builder.CreateString(osFix.str());
        indexFixing = frc->indexFixing();
        spread = frc->spread();
    }

    const double discount = discountCurve->discount(coupon->date());
    const double amount = coupon->amount();

    SwapLegFlowBuilder fb(builder);
    fb.add_payment_date(paymentDate);
    fb.add_accrual_start_date(accrualStart);
    fb.add_accrual_end_date(accrualEnd);
    fb.add_amount(amount);
    fb.add_discount(discount);
    fb.add_present_value(amount * discount);
    fb.add_rate(coupon->rate());
    if (frc) {
        fb.add_fixing_date(fixingDate);
        fb.add_index_fixing(indexFixing);
        fb.add_spread(spread);
    }
    return fb.Finish();
}

} // namespace quantra
