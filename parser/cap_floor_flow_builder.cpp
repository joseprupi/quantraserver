#include "cap_floor_flow_builder.h"

#include <ql/cashflows/iborcoupon.hpp>
#include <ql/utilities/dataformatters.hpp>
#include "common.h"

namespace quantra {

std::vector<flatbuffers::Offset<CapFloorLet>> buildCapFloorDetails(
    const QuantLib::Leg& floatingLeg,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const QuantLib::Date& asOf) {
    std::vector<flatbuffers::Offset<CapFloorLet>> out;
    for (const auto& cf : floatingLeg) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::IborCoupon>(cf);
        if (!coupon || coupon->hasOccurred(asOf)) {
            continue;
        }

        std::ostringstream osPayment, osStart, osEnd, osFixing;
        osPayment << DateToIso(coupon->date());
        osStart << DateToIso(coupon->accrualStartDate());
        osEnd << DateToIso(coupon->accrualEndDate());
        osFixing << DateToIso(coupon->fixingDate());

        CapFloorLetBuilder b(*builder);
        b.add_payment_date(builder->CreateString(osPayment.str()));
        b.add_accrual_start_date(builder->CreateString(osStart.str()));
        b.add_accrual_end_date(builder->CreateString(osEnd.str()));
        b.add_fixing_date(builder->CreateString(osFixing.str()));
        b.add_forward_rate(coupon->indexFixing());
        b.add_discount(discountCurve->discount(coupon->date()));
        out.push_back(b.Finish());
    }
    return out;
}

} // namespace quantra
