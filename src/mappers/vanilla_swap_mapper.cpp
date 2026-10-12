#include "vanilla_swap_mapper.h"

#include "error.h"
#include "swap_leg_mapper.h"

#include <cmath>

namespace quantra {

namespace {

/// `VanillaSwap.fixed_leg` takes FixedCoupons, `floating_leg` IborCoupons or
/// CmsCoupons; per-period notionals are accepted on both, the exchange flags
/// are optional (absent = false).
SwapLegRules fixedLegRules() {
    SwapLegRules rules;
    rules.allowedKinds = {SwapLegData::Kind::Fixed};
    rules.notionalsAllowed = true;
    rules.endpoint = "price-vanilla-swap";
    return rules;
}

SwapLegRules floatingLegRules() {
    SwapLegRules rules;
    rules.allowedKinds = {SwapLegData::Kind::Ibor, SwapLegData::Kind::Cms};
    rules.notionalsAllowed = true;
    rules.endpoint = "price-vanilla-swap";
    return rules;
}

VanillaSwapTrade extractTrade(const quantra::PriceVanillaSwap* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwap entry is null");
    }
    const auto* swap = pricing->vanilla_swap();
    if (!swap) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwap.vanilla_swap is required");
    }
    VanillaSwapTrade trade;
    trade.fixedLeg = DecodeSwapLeg(swap->fixed_leg(), "VanillaSwap.fixed_leg", fixedLegRules());
    trade.floatingLeg =
        DecodeSwapLeg(swap->floating_leg(), "VanillaSwap.floating_leg", floatingLegRules());
    RequireOppositeSides(
        trade.fixedLeg, "VanillaSwap.fixed_leg", trade.floatingLeg, "VanillaSwap.floating_leg");
    RequireOneCurrency(
        trade.fixedLeg, "VanillaSwap.fixed_leg", trade.floatingLeg, "VanillaSwap.floating_leg",
        "price-vanilla-swap");
    return trade;
}

} // namespace

VanillaSwapInputs VanillaSwapMapper::toInputs(const quantra::PriceVanillaSwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("PriceVanillaSwapRequest.swaps is required and must be non-empty");
    }

    VanillaSwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceVanillaSwapResponse> VanillaSwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const VanillaSwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::VanillaSwapResponse>> rows;
    rows.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        auto legs = SerializeSwapLegResults(builder, swap.legs, swap.includeFlows);

        quantra::VanillaSwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        if (swap.fairRate) rb.add_fair_rate(*swap.fairRate);
        if (swap.fairSpread) rb.add_fair_spread(*swap.fairSpread);
        rb.add_legs(legs);
        // The CMS diagnostics block exists only for a CMS floating leg; an
        // unset optional leaves every `used_cms_*` field absent.
        if (swap.cmsUsed) {
            const CmsPricerUsed& used = *swap.cmsUsed;
            rb.add_used_cms_pricer_type(used.pricerType);
            rb.add_used_cms_yield_curve_model(used.yieldCurveModel);
            rb.add_used_cms_mean_reversion(used.meanReversion);
            rb.add_used_cms_hagan_lower_limit(used.haganLowerLimit);
            rb.add_used_cms_hagan_upper_limit(used.haganUpperLimit);
            rb.add_used_cms_hagan_precision(used.haganPrecision);
            rb.add_used_cms_hagan_hard_upper_limit(used.haganHardUpperLimit);
        }
        rows.push_back(rb.Finish());
    }

    auto rowsVec = builder.CreateVector(rows);
    quantra::PriceVanillaSwapResponseBuilder rb(builder);
    rb.add_swaps(rowsVec);
    return rb.Finish();
}

} // namespace quantra
