#include "vanilla_swap_flow_builder.h"

#include <limits>

#include <ql/cashflows/cmscoupon.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/utilities/dataformatters.hpp>
#include "date_convert.h"

namespace quantra {

VanillaSwapFlowsBuildResult buildVanillaSwapFlows(
    const QuantLib::Leg& fixedLeg,
    const QuantLib::Leg& floatingLeg,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const QuantLib::Date& asOf) {
    VanillaSwapFlowsBuildResult out;

    for (const auto& cf : fixedLeg) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::FixedRateCoupon>(cf);
        if (coupon && !coupon->hasOccurred(asOf)) {
            std::ostringstream osPayment, osStart, osEnd;
            osPayment << DateToIso(coupon->date());
            osStart << DateToIso(coupon->accrualStartDate());
            osEnd << DateToIso(coupon->accrualEndDate());

            auto paymentDate = builder->CreateString(osPayment.str());
            auto accrualStart = builder->CreateString(osStart.str());
            auto accrualEnd = builder->CreateString(osEnd.str());

            const double discount = discountCurve->discount(coupon->date());
            const double pv = coupon->amount() * discount;

            SwapLegFlowBuilder fb(*builder);
            fb.add_payment_date(paymentDate);
            fb.add_accrual_start_date(accrualStart);
            fb.add_accrual_end_date(accrualEnd);
            fb.add_amount(coupon->amount());
            fb.add_accrual_year_fraction(coupon->accrualPeriod());
            fb.add_gearing(1.0);
            fb.add_discount(discount);
            fb.add_present_value(pv);
            fb.add_rate(coupon->rate());
            out.fixedFlows.push_back(fb.Finish());
        }
    }

    for (const auto& cf : floatingLeg) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(cf);
        if (coupon && !coupon->hasOccurred(asOf)) {
            std::ostringstream osPayment, osStart, osEnd, osFixing;
            osPayment << DateToIso(coupon->date());
            osStart << DateToIso(coupon->accrualStartDate());
            osEnd << DateToIso(coupon->accrualEndDate());
            osFixing << DateToIso(coupon->fixingDate());

            auto paymentDate = builder->CreateString(osPayment.str());
            auto accrualStart = builder->CreateString(osStart.str());
            auto accrualEnd = builder->CreateString(osEnd.str());
            auto fixingDate = builder->CreateString(osFixing.str());

            const double discount = discountCurve->discount(coupon->date());
            const double pv = coupon->amount() * discount;

            SwapLegFlowBuilder fb(*builder);
            fb.add_payment_date(paymentDate);
            fb.add_accrual_start_date(accrualStart);
            fb.add_accrual_end_date(accrualEnd);
            fb.add_amount(coupon->amount());
            fb.add_accrual_year_fraction(coupon->accrualPeriod());
            fb.add_gearing(coupon->gearing());
            fb.add_discount(discount);
            fb.add_present_value(pv);
            fb.add_fixing_date(fixingDate);

            double fixing = std::numeric_limits<double>::quiet_NaN();
            bool hasCmsSwapRate = false;
            double cmsSwapRate = 0.0;
            try {
                fixing = coupon->indexFixing();
                if (std::dynamic_pointer_cast<QuantLib::CmsCoupon>(coupon)) {
                    hasCmsSwapRate = true;
                    cmsSwapRate = fixing;
                }
            } catch (...) {
            }

            fb.add_index_fixing(fixing);
            fb.add_has_cms_swap_rate(hasCmsSwapRate);
            if (hasCmsSwapRate) {
                fb.add_cms_swap_rate(cmsSwapRate);
            }
            fb.add_spread(coupon->spread());
            fb.add_rate(coupon->rate());
            out.floatingFlows.push_back(fb.Finish());
        }
    }

    return out;
}

} // namespace quantra
