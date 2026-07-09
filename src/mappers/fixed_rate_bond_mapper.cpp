#include "fixed_rate_bond_mapper.h"

#include "enum_convert.h"
#include "error.h"
#include "fixed_rate_bond_parser.h"

namespace quantra {

FixedRateBondInputs FixedRateBondMapper::toInputs(
    const quantra::PriceFixedRateBondRequest* req) const {

    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceFixedRateBondRequest is null");
    }
    const auto* bondPricings = req->bonds();
    if (bondPricings == nullptr || bondPricings->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("PriceFixedRateBondRequest.bonds is required and must be non-empty");
    }

    FixedRateBondParser bondParser;
    FixedRateBondInputs inputs;
    inputs.trades.reserve(bondPricings->size());

    for (auto it = bondPricings->begin(); it != bondPricings->end(); ++it) {
        if (it->fixed_rate_bond() == nullptr) {
            QUANTRA_INVALID_ARGUMENT("PriceFixedRateBond.fixed_rate_bond is required");
        }
        if (it->discounting_curve() == nullptr) {
            QUANTRA_INVALID_ARGUMENT("PriceFixedRateBond.discounting_curve is required");
        }
        if (it->yield() == nullptr) {
            QUANTRA_INVALID_ARGUMENT("PriceFixedRateBond.yield is required");
        }

        FixedRateBondTrade trade;
        trade.bond = bondParser.parse(it->fixed_rate_bond());
        trade.discountingCurveId = it->discounting_curve()->str();
        if (!it->yield()->day_counter().has_value()) {
            QUANTRA_INVALID_ARGUMENT("Yield.day_counter is required");
        }
        if (!it->yield()->compounding().has_value()) {
            QUANTRA_INVALID_ARGUMENT("Yield.compounding is required");
        }
        if (!it->yield()->frequency().has_value()) {
            QUANTRA_INVALID_ARGUMENT("Yield.frequency is required");
        }
        trade.yieldDc = DayCounterToQL(it->yield()->day_counter().value());
        trade.yieldComp = CompoundingToQL(it->yield()->compounding().value());
        trade.yieldFreq = FrequencyToQL(it->yield()->frequency().value());
        inputs.trades.push_back(std::move(trade));
    }

    return inputs;
}

namespace {

/**
 * Serialize one plain flow into a FlowsWrapper offset. Mirrors the existing
 * buildFixedBondFlows shape verbatim — including the legacy quirk that past
 * coupons are tagged Flow_FlowPastInterest while the payload is laid out as
 * FlowInterest (no fixing_date, no discount, no price). Keeping this here
 * (instead of reusing bond_flow_builder) means the boundary stays clean:
 * pricer is FB-free, and bond_flow_builder remains untouched.
 */
flatbuffers::Offset<quantra::FlowsWrapper> serializeFlow(
    flatbuffers::grpc::MessageBuilder& builder,
    const FixedRateBondFlowPlain& f) {
    using Kind = FixedRateBondFlowPlain::Kind;
    switch (f.kind) {
        case Kind::Interest: {
            auto accrualStart = builder.CreateString(f.accrualStartDate);
            auto accrualEnd = builder.CreateString(f.accrualEndDate);
            quantra::FlowInterestBuilder fib(builder);
            fib.add_amount(f.amount);
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
            // Intentional: matches the original buildFixedBondFlows path,
            // which uses FlowInterestBuilder + Flow_FlowPastInterest tag.
            quantra::FlowInterestBuilder fib(builder);
            fib.add_amount(f.amount);
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

flatbuffers::Offset<quantra::PriceFixedRateBondResponse> FixedRateBondMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const FixedRateBondResult& result) const {

    std::vector<flatbuffers::Offset<quantra::FixedRateBondResponse>> bondsVector;
    bondsVector.reserve(result.bonds.size());

    for (const auto& bond : result.bonds) {
        std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> flowOffsets;
        flowOffsets.reserve(bond.flows.size());
        for (const auto& flow : bond.flows) {
            flowOffsets.push_back(serializeFlow(builder, flow));
        }
        auto flows = builder.CreateVector(flowOffsets);

        quantra::FixedRateBondResponseBuilder rb(builder);
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
    quantra::PriceFixedRateBondResponseBuilder rb(builder);
    rb.add_bonds(bonds);
    return rb.Finish();
}

} // namespace quantra
