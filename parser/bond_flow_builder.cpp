#include "bond_flow_builder.h"

#include <ql/cashflow.hpp>
#include <ql/cashflows/fixedratecoupon.hpp>
#include <ql/cashflows/floatingratecoupon.hpp>
#include <ql/utilities/dataformatters.hpp>
#include "common.h"

namespace quantra {

namespace {

flatbuffers::Offset<flatbuffers::String> makeDate(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const QuantLib::Date& d) {
    return builder->CreateString(DateToIso(d));
}

} // namespace

std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> buildFixedBondFlows(
    const QuantLib::Leg& cashflows,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const QuantLib::Date& asOf) {
    std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> flows;
    for (const auto& cf : cashflows) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::FixedRateCoupon>(cf);
        if (coupon) {
            if (!coupon->hasOccurred(asOf)) {
                auto accrualStartDate = makeDate(builder, coupon->accrualStartDate());
                auto accrualEndDate = makeDate(builder, coupon->accrualEndDate());
                auto flowInterestBuilder = quantra::FlowInterestBuilder(*builder);
                flowInterestBuilder.add_amount(coupon->amount());
                flowInterestBuilder.add_accrual_start_date(accrualStartDate);
                flowInterestBuilder.add_accrual_end_date(accrualEndDate);
                flowInterestBuilder.add_rate(coupon->rate());
                flowInterestBuilder.add_discount(discountCurve->discount(coupon->date()));
                flowInterestBuilder.add_price(coupon->amount() * discountCurve->discount(coupon->date()));
                auto flow = flowInterestBuilder.Finish();
                quantra::FlowsWrapperBuilder fb(*builder);
                fb.add_flow_type(quantra::Flow_FlowInterest);
                fb.add_flow(flow.Union());
                flows.push_back(fb.Finish());
            } else {
                auto accrualStartDate = makeDate(builder, coupon->accrualStartDate());
                auto accrualEndDate = makeDate(builder, coupon->accrualEndDate());
                auto flowPastBuilder = quantra::FlowInterestBuilder(*builder);
                flowPastBuilder.add_amount(coupon->amount());
                flowPastBuilder.add_accrual_start_date(accrualStartDate);
                flowPastBuilder.add_accrual_end_date(accrualEndDate);
                flowPastBuilder.add_rate(coupon->rate());
                auto flow = flowPastBuilder.Finish();
                quantra::FlowsWrapperBuilder fb(*builder);
                fb.add_flow_type(quantra::Flow_FlowPastInterest);
                fb.add_flow(flow.Union());
                flows.push_back(fb.Finish());
            }
            continue;
        }

        auto cash = std::dynamic_pointer_cast<QuantLib::CashFlow>(cf);
        if (cash && !cash->hasOccurred(asOf)) {
            auto paymentDate = makeDate(builder, cash->date());
            auto flowNotionalBuilder = quantra::FlowNotionalBuilder(*builder);
            flowNotionalBuilder.add_amount(cash->amount());
            flowNotionalBuilder.add_date(paymentDate);
            flowNotionalBuilder.add_discount(discountCurve->discount(cash->date()));
            flowNotionalBuilder.add_price(cash->amount() * discountCurve->discount(cash->date()));
            auto flow = flowNotionalBuilder.Finish();
            quantra::FlowsWrapperBuilder fb(*builder);
            fb.add_flow_type(quantra::Flow_FlowNotional);
            fb.add_flow(flow.Union());
            flows.push_back(fb.Finish());
        }
    }
    return flows;
}

std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> buildFloatingBondFlows(
    const QuantLib::Leg& cashflows,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const QuantLib::Date& asOf) {
    std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> flows;
    for (const auto& cf : cashflows) {
        auto coupon = std::dynamic_pointer_cast<QuantLib::FloatingRateCoupon>(cf);
        if (coupon) {
            if (!coupon->hasOccurred(asOf)) {
                auto accrualStartDate = makeDate(builder, coupon->accrualStartDate());
                auto accrualEndDate = makeDate(builder, coupon->accrualEndDate());
                auto flowInterestBuilder = quantra::FlowInterestBuilder(*builder);
                flowInterestBuilder.add_amount(coupon->amount());
                flowInterestBuilder.add_fixing_date(coupon->indexFixing());
                flowInterestBuilder.add_accrual_start_date(accrualStartDate);
                flowInterestBuilder.add_accrual_end_date(accrualEndDate);
                flowInterestBuilder.add_rate(coupon->rate());
                flowInterestBuilder.add_discount(discountCurve->discount(coupon->date()));
                flowInterestBuilder.add_price(coupon->amount() * discountCurve->discount(coupon->date()));
                auto flow = flowInterestBuilder.Finish();
                quantra::FlowsWrapperBuilder fb(*builder);
                fb.add_flow_type(quantra::Flow_FlowInterest);
                fb.add_flow(flow.Union());
                flows.push_back(fb.Finish());
            } else {
                auto accrualStartDate = makeDate(builder, coupon->accrualStartDate());
                auto accrualEndDate = makeDate(builder, coupon->accrualEndDate());
                auto flowPastBuilder = quantra::FlowPastInterestBuilder(*builder);
                flowPastBuilder.add_amount(coupon->amount());
                flowPastBuilder.add_fixing_date(coupon->indexFixing());
                flowPastBuilder.add_accrual_start_date(accrualStartDate);
                flowPastBuilder.add_accrual_end_date(accrualEndDate);
                flowPastBuilder.add_rate(coupon->rate());
                auto flow = flowPastBuilder.Finish();
                quantra::FlowsWrapperBuilder fb(*builder);
                fb.add_flow_type(quantra::Flow_FlowPastInterest);
                fb.add_flow(flow.Union());
                flows.push_back(fb.Finish());
            }
            continue;
        }

        auto cash = std::dynamic_pointer_cast<QuantLib::CashFlow>(cf);
        if (cash && !cash->hasOccurred(asOf)) {
            auto paymentDate = makeDate(builder, cash->date());
            auto flowNotionalBuilder = quantra::FlowNotionalBuilder(*builder);
            flowNotionalBuilder.add_amount(cash->amount());
            flowNotionalBuilder.add_date(paymentDate);
            flowNotionalBuilder.add_discount(discountCurve->discount(cash->date()));
            flowNotionalBuilder.add_price(cash->amount() * discountCurve->discount(cash->date()));
            auto flow = flowNotionalBuilder.Finish();
            quantra::FlowsWrapperBuilder fb(*builder);
            fb.add_flow_type(quantra::Flow_FlowNotional);
            fb.add_flow(flow.Union());
            flows.push_back(fb.Finish());
        }
    }
    return flows;
}

} // namespace quantra
