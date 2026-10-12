#include "cross_currency_swap_mapper.h"

#include "error.h"
#include "fx_spot_parser.h" // RequireIsoCurrency
#include "swap_leg_mapper.h"

#include <cmath>

namespace quantra {

namespace {

std::string requireId(const flatbuffers::String* value, const std::string& field) {
    if (!value || value->str().empty()) {
        QUANTRA_INVALID_ARGUMENT(field + " is required");
    }
    return value->str();
}

/// Either `CrossCurrencySwap` leg takes FixedCoupons, IborCoupons or
/// OvernightCoupons (no CmsCoupons in this version), constant notional only,
/// and both exchange flags presence-required.
SwapLegRules legRules() {
    SwapLegRules rules;
    rules.allowedKinds = {
        SwapLegData::Kind::Fixed, SwapLegData::Kind::Ibor, SwapLegData::Kind::Overnight};
    rules.exchangesRequired = true;
    rules.endpoint = "price-cross-currency-swap";
    return rules;
}

CrossCurrencySwapTrade extractTrade(const quantra::PriceCrossCurrencySwap* pricing) {
    if (pricing == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCrossCurrencySwap entry is null");
    }
    const auto* swap = pricing->cross_currency_swap();
    if (!swap) {
        QUANTRA_INVALID_ARGUMENT("PriceCrossCurrencySwap.cross_currency_swap is required");
    }

    CrossCurrencySwapTrade trade;
    trade.legs.reserve(2);
    trade.legs.push_back(DecodeSwapLeg(swap->leg1(), "CrossCurrencySwap.leg1", legRules()));
    trade.legs.push_back(DecodeSwapLeg(swap->leg2(), "CrossCurrencySwap.leg2", legRules()));
    const SwapLegData& l0 = trade.legs[0];
    const SwapLegData& l1 = trade.legs[1];
    RequireOppositeSides(l0, "CrossCurrencySwap.leg1", l1, "CrossCurrencySwap.leg2");
    if (l0.currency == l1.currency) {
        QUANTRA_INVALID_ARGUMENT(
            "CrossCurrencySwap.leg1.currency and CrossCurrencySwap.leg2.currency must differ, "
            "both are '" +
            l0.currency +
            "' (two legs in one currency are a vanilla, OIS or basis swap: use "
            "price-vanilla-swap / price-ois-swap / price-basis-swap)");
    }
    trade.fxSpotId = requireId(pricing->fx_spot(), "PriceCrossCurrencySwap.fx_spot");
    trade.reportingCurrency = RequireIsoCurrency(
        pricing->reporting_currency(), "PriceCrossCurrencySwap.reporting_currency");
    if (trade.reportingCurrency != l0.currency && trade.reportingCurrency != l1.currency) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCrossCurrencySwap.reporting_currency '" + trade.reportingCurrency + "' must be " +
            l0.currency + " or " + l1.currency +
            " (reporting in a third currency is not supported)");
    }
    return trade;
}

} // namespace

CrossCurrencySwapInputs CrossCurrencySwapMapper::toInputs(
    const quantra::PriceCrossCurrencySwapRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("PriceCrossCurrencySwapRequest is null");
    }
    const auto* swaps = req->swaps();
    if (swaps == nullptr || swaps->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(
            "PriceCrossCurrencySwapRequest.swaps is required and must be non-empty");
    }

    CrossCurrencySwapInputs inputs;
    inputs.includeFlows = req->include_flows();
    inputs.trades.reserve(swaps->size());
    for (auto it = swaps->begin(); it != swaps->end(); ++it) {
        inputs.trades.push_back(extractTrade(*it));
    }
    return inputs;
}

flatbuffers::Offset<quantra::PriceCrossCurrencySwapResponse> CrossCurrencySwapMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder, const CrossCurrencySwapResult& result) const {

    std::vector<flatbuffers::Offset<quantra::CrossCurrencySwapResponse>> rows;
    rows.reserve(result.swaps.size());

    for (const auto& swap : result.swaps) {
        auto legs = SerializeSwapLegResults(builder, swap.legs, swap.includeFlows);
        auto spotDate = builder.CreateString(swap.spotDate);

        quantra::CrossCurrencySwapResponseBuilder rb(builder);
        rb.add_npv(swap.npv);
        rb.add_legs(legs);
        // Optional by construction: present only when the swap shape defines
        // them (one fixed leg -> fair_rate; no fixed leg -> fair_spread).
        if (swap.fairRate && std::isfinite(*swap.fairRate)) rb.add_fair_rate(*swap.fairRate);
        if (swap.fairSpread && std::isfinite(*swap.fairSpread)) {
            rb.add_fair_spread(*swap.fairSpread);
        }
        rb.add_fx_spot_used(swap.fxSpotUsed);
        rb.add_fx_as_of_rate(swap.fxAsOfRate);
        rb.add_spot_date(spotDate);
        rows.push_back(rb.Finish());
    }

    auto rowsVec = builder.CreateVector(rows);
    quantra::PriceCrossCurrencySwapResponseBuilder rb(builder);
    rb.add_swaps(rowsVec);
    return rb.Finish();
}

} // namespace quantra
