#include "basis_swap_mapper.h"

#include "error.h"
#include "swap_leg_mapper.h"

namespace quantra {

namespace {

/// Both `BasisSwap` legs take IborCoupons or OvernightCoupons; no per-period
/// notionals, exchange flags optional (absent = false).
SwapLegRules legRules() {
    SwapLegRules rules;
    rules.allowedKinds = {SwapLegData::Kind::Ibor, SwapLegData::Kind::Overnight};
    rules.endpoint = "price-basis-swap";
    return rules;
}

BasisSwapTrade extractTrade(const quantra::PriceBasisSwap* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwap entry is null");
    }
    const auto* swap = pricing->basis_swap();
    if (!swap) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwap.basis_swap is required");
    }
    BasisSwapTrade trade;
    trade.leg1 = DecodeSwapLeg(swap->leg1(), "BasisSwap.leg1", legRules());
    trade.leg2 = DecodeSwapLeg(swap->leg2(), "BasisSwap.leg2", legRules());
    RequireOppositeSides(trade.leg1, "BasisSwap.leg1", trade.leg2, "BasisSwap.leg2");
    RequireOneCurrency(
        trade.leg1, "BasisSwap.leg1", trade.leg2, "BasisSwap.leg2", "price-basis-swap");
    return trade;
}

} // namespace

BasisSwapInputs BasisSwapMapper::toInputs(const quantra::PriceBasisSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("PriceBasisSwapRequest.swaps is required and must be non-empty");
    }

    BasisSwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceBasisSwapResponse> BasisSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const BasisSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::BasisSwapResponse>> rows;
    rows.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        auto legs = SerializeSwapLegResults(builder, swap.legs, swap.includeFlows);

        quantra::BasisSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        if (swap.fairSpread) rb.add_fair_spread(*swap.fairSpread);
        rb.add_legs(legs);
        rows.push_back(rb.Finish());
    }

    auto rowsVec = builder.CreateVector(rows);
    quantra::PriceBasisSwapResponseBuilder rb(builder);
    rb.add_swaps(rowsVec);
    return rb.Finish();
}

} // namespace quantra
