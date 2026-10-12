#include "ois_swap_mapper.h"

#include "error.h"
#include "swap_leg_mapper.h"

namespace quantra {

namespace {

/// `OisSwap.fixed_leg` takes FixedCoupons, `overnight_leg` OvernightCoupons;
/// both legs state their `payment_calendar` / `payment_lag` (an OIS pays
/// with the overnight calendar and lag); no per-period notionals, exchange
/// flags optional (absent = false).
SwapLegRules legRules(SwapLegData::Kind kind) {
    SwapLegRules rules;
    rules.allowedKinds = {kind};
    rules.paymentLagRequired = true;
    rules.endpoint = "price-ois-swap";
    return rules;
}

OisSwapTrade extractTrade(const quantra::PriceOisSwap* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwap entry is null");
    }
    const auto* swap = pricing->ois_swap();
    if (!swap) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwap.ois_swap is required");
    }
    OisSwapTrade trade;
    trade.fixedLeg =
        DecodeSwapLeg(swap->fixed_leg(), "OisSwap.fixed_leg", legRules(SwapLegData::Kind::Fixed));
    trade.overnightLeg = DecodeSwapLeg(
        swap->overnight_leg(), "OisSwap.overnight_leg", legRules(SwapLegData::Kind::Overnight));
    RequireOppositeSides(
        trade.fixedLeg, "OisSwap.fixed_leg", trade.overnightLeg, "OisSwap.overnight_leg");
    RequireOneCurrency(
        trade.fixedLeg, "OisSwap.fixed_leg", trade.overnightLeg, "OisSwap.overnight_leg",
        "price-ois-swap");
    return trade;
}

} // namespace

OisSwapInputs OisSwapMapper::toInputs(const quantra::PriceOisSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("PriceOisSwapRequest.swaps is required and must be non-empty");
    }

    OisSwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceOisSwapResponse> OisSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const OisSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::OisSwapResponse>> rows;
    rows.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        auto legs = SerializeSwapLegResults(builder, swap.legs, swap.includeFlows);

        quantra::OisSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        if (swap.fairRate) rb.add_fair_rate(*swap.fairRate);
        rb.add_legs(legs);
        rows.push_back(rb.Finish());
    }

    auto rowsVec = builder.CreateVector(rows);
    quantra::PriceOisSwapResponseBuilder rb(builder);
    rb.add_swaps(rowsVec);
    return rb.Finish();
}

} // namespace quantra
