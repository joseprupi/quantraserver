#include "floating_rate_bond_mapper.h"

#include <cstdint>

#include <ql/types.hpp>

#include "common.h"
#include "common_parser.h"
#include "enums.h"
#include "error.h"

namespace quantra {

namespace {

FloatingRateBondTrade extractTrade(const quantra::PriceFloatingRateBond* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceFloatingRateBond entry is null");
    }
    if (!pricing->floating_rate_bond()) {
        QUANTRA_INVALID_ARGUMENT("PriceFloatingRateBond.floating_rate_bond is required");
    }
    if (!pricing->discounting_curve() || pricing->discounting_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceFloatingRateBond.discounting_curve is required");
    }
    if (!pricing->forwarding_curve() || pricing->forwarding_curve()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceFloatingRateBond.forwarding_curve is required");
    }
    if (!pricing->coupon_pricer() || pricing->coupon_pricer()->str().empty()) {
        QUANTRA_INVALID_ARGUMENT("PriceFloatingRateBond.coupon_pricer is required");
    }
    // `yield` is only consumed by the analytics block (bondPricingDetails) —
    // matches the legacy handler which only dereferences it under that flag.

    const auto* fbBond = pricing->floating_rate_bond();
    if (!fbBond->index() || !fbBond->index()->id()) {
        QUANTRA_INVALID_ARGUMENT("FloatingRateBond.index.id is required");
    }
    if (!fbBond->schedule()) {
        QUANTRA_INVALID_ARGUMENT("FloatingRateBond.schedule is required");
    }
    if (!fbBond->issue_date()) {
        QUANTRA_INVALID_ARGUMENT("FloatingRateBond.issue_date is required");
    }

    ScheduleParser scheduleParser;
    YieldParser yieldParser;

    FloatingRateBondTrade trade;
    trade.settlement_days = fbBond->settlement_days();
    trade.face_amount = fbBond->face_amount();
    trade.schedule = *scheduleParser.parse(fbBond->schedule());
    trade.accrual_day_counter = DayCounterToQL(fbBond->accrual_day_counter());
    trade.payment_convention = ConventionToQL(fbBond->payment_convention());
    trade.fixing_days = fbBond->fixing_days();
    trade.spread = fbBond->spread();
    trade.in_arrears = fbBond->in_arrears();
    trade.redemption = fbBond->redemption();
    trade.issue_date = DateToQL(fbBond->issue_date()->str());

    trade.indexId = fbBond->index()->id()->str();
    trade.discountingCurveId = pricing->discounting_curve()->str();
    trade.forwardingCurveId = pricing->forwarding_curve()->str();
    trade.couponPricerId = pricing->coupon_pricer()->str();

    if (pricing->yield()) {
        auto yieldStruct = yieldParser.parse(pricing->yield());
        trade.yieldDc = yieldStruct->day_counter;
        trade.yieldComp = yieldStruct->compounding;
        trade.yieldFreq = yieldStruct->frequency;
    }
    return trade;
}

/**
 * Serialize one plain flow into a FlowsWrapper offset. Mirrors the existing
 * buildFloatingBondFlows shape verbatim — including the legacy quirk that the
 * floating-coupon `fixing_date` slot (typed `string` in the schema) is written
 * via the implicit double→uint32→Offset<String> conversion of the underlying
 * FloatingRateCoupon::indexFixing() value. Keeping this here (instead of
 * reusing bond_flow_builder) means the pricer stays FB-free.
 */
flatbuffers::Offset<quantra::FlowsWrapper> serializeFlow(
    flatbuffers::grpc::MessageBuilder& builder,
    const FloatingRateBondFlowPlain& f) {
    using Kind = FloatingRateBondFlowPlain::Kind;
    switch (f.kind) {
        case Kind::Interest: {
            auto accrualStart = builder.CreateString(f.accrualStartDate);
            auto accrualEnd = builder.CreateString(f.accrualEndDate);
            quantra::FlowInterestBuilder fib(builder);
            fib.add_amount(f.amount);
            fib.add_fixing_date(flatbuffers::Offset<flatbuffers::String>(
                static_cast<std::uint32_t>(f.indexFixing)));
            fib.add_accrual_start_date(accrualStart);
            fib.add_accrual_end_date(accrualEnd);
            fib.add_rate(f.rate);
            fib.add_discount(f.discount);
            fib.add_price(f.price);
            auto flow = fib.Finish();
            quantra::FlowsWrapperBuilder wb(builder);
            wb.add_flow_type(quantra::Flow_FlowInterest);
            wb.add_flow(flow.Union());
            return wb.Finish();
        }
        case Kind::PastInterest: {
            auto accrualStart = builder.CreateString(f.accrualStartDate);
            auto accrualEnd = builder.CreateString(f.accrualEndDate);
            // Legacy buildFloatingBondFlows uses FlowPastInterestBuilder here
            // (the fixed-rate path uses FlowInterestBuilder + the past tag —
            // a separate quirk preserved in fixed_rate_bond_mapper).
            quantra::FlowPastInterestBuilder fib(builder);
            fib.add_amount(f.amount);
            fib.add_fixing_date(flatbuffers::Offset<flatbuffers::String>(
                static_cast<std::uint32_t>(f.indexFixing)));
            fib.add_accrual_start_date(accrualStart);
            fib.add_accrual_end_date(accrualEnd);
            fib.add_rate(f.rate);
            auto flow = fib.Finish();
            quantra::FlowsWrapperBuilder wb(builder);
            wb.add_flow_type(quantra::Flow_FlowPastInterest);
            wb.add_flow(flow.Union());
            return wb.Finish();
        }
        case Kind::Notional: {
            auto paymentDate = builder.CreateString(f.paymentDate);
            quantra::FlowNotionalBuilder nb(builder);
            nb.add_amount(f.amount);
            nb.add_date(paymentDate);
            nb.add_discount(f.discount);
            nb.add_price(f.price);
            auto flow = nb.Finish();
            quantra::FlowsWrapperBuilder wb(builder);
            wb.add_flow_type(quantra::Flow_FlowNotional);
            wb.add_flow(flow.Union());
            return wb.Finish();
        }
    }
    // Unreachable; keep the compiler quiet.
    quantra::FlowsWrapperBuilder wb(builder);
    return wb.Finish();
}

} // namespace

FloatingRateBondInputs FloatingRateBondMapper::toInputs(
    const quantra::PriceFloatingRateBondRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceFloatingRateBondRequest is null");
    }
    const auto* bondPricings = req->bonds();
    if (bondPricings == nullptr || bondPricings->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceFloatingRateBondRequest.bonds is required and must be non-empty");
    }

    FloatingRateBondInputs inputs;
    inputs.trades.reserve(bondPricings->size());
    for (auto it = bondPricings->begin(); it != bondPricings->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceFloatingRateBondResponse> FloatingRateBondMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const FloatingRateBondResult& result) const {

    std::vector<flatbuffers::Offset<quantra::FloatingRateBondResponse>> bondsVector;
    bondsVector.reserve(result.bonds.size());

    for (const auto& bond : result.bonds) {
        std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> flowOffsets;
        flowOffsets.reserve(bond.flows.size());
        for (const auto& flow : bond.flows) {
            flowOffsets.push_back(serializeFlow(builder, flow));
        }
        auto flows = builder.CreateVector(flowOffsets);

        quantra::FloatingRateBondResponseBuilder rb(builder);
        rb.add_flows(flows);
        rb.add_npv(bond.npv);
        if (bond.hasDetails) {
            rb.add_clean_price(bond.cleanPrice);
            rb.add_dirty_price(bond.dirtyPrice);
            rb.add_accrued_amount(bond.accruedAmount);
            rb.add_yield(bond.yield);
            rb.add_accrued_days(bond.accruedDays);
            rb.add_modified_duration(bond.modifiedDuration);
            rb.add_macaulay_duration(bond.macaulayDuration);
            rb.add_convexity(bond.convexity);
            rb.add_bps(bond.bps);
        }
        bondsVector.push_back(rb.Finish());
    }

    auto bonds = builder.CreateVector(bondsVector);
    quantra::PriceFloatingRateBondResponseBuilder rb(builder);
    rb.add_bonds(bonds);
    return rb.Finish();
}

} // namespace quantra
