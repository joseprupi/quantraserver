#ifndef QUANTRA_SWAP_LEG_MAPPER_H
#define QUANTRA_SWAP_LEG_MAPPER_H

/**
 * The one place a wire `SwapLeg` is decoded and a `SwapLegResult` encoded.
 * Every swap endpoint (vanilla, OIS, basis, cross-currency) and the swaption
 * underlying lift their legs through DecodeSwapLeg, each with its own
 * SwapLegRules (which coupon kinds the role accepts, whether the exchange
 * flags are presence-required, whether per-period notionals are allowed), so
 * the field checks and their 400 messages are the same everywhere and the
 * leg reaches the evaluators as the same plain SwapLegData.
 */

#include "swap_leg_data.h"
#include "swap_leg_pricer.h" // SwapLegPerLeg

#include "swap_leg_generated.h"

#include "flatbuffers/grpc.h"

#include <string>
#include <vector>

namespace quantra {

/// What a given leg role accepts.
struct SwapLegRules {
    /// Coupon kinds the role accepts, in the order the 400 lists them.
    std::vector<SwapLegData::Kind> allowedKinds;
    /// Presence-required exchange flags (cross-currency) versus absent =
    /// false (single-currency endpoints).
    bool exchangesRequired = false;
    /// Whether an explicit `true` exchange flag is accepted at all (a swaption
    /// underlying cannot carry principal exchanges).
    bool exchangesAllowed = true;
    /// Whether a per-period `notionals` vector is accepted (vanilla swap
    /// legs) or a 400 naming the endpoint.
    bool notionalsAllowed = false;
    /// Whether `payment_calendar` and `payment_lag` are presence-required
    /// (both legs of an OIS) or optional (absent = the schedule's calendar,
    /// no lag).
    bool paymentLagRequired = false;
    /// Endpoint name for the messages that mention it ("price-ois-swap").
    std::string endpoint;
};

/// Decode `fb` (the wire field `name`, e.g. "VanillaSwap.fixed_leg") under
/// `rules`. Every check that needs no market data happens here, as a 400
/// naming the field: presence of `pay_receive`, `currency`, `coupons` (and
/// its kind against the role), `discounting_curve`; `forwarding_curve`
/// required for floating kinds and rejected for FixedCoupons; the exchange
/// flags; the shared leg fields (schedule, notional(s), day count, payment
/// conventions) and every kind-specific coupon field.
SwapLegData DecodeSwapLeg(
    const quantra::SwapLeg* fb, const std::string& name, const SwapLegRules& rules);

/// 400 unless one leg is paid and the other received.
void RequireOppositeSides(
    const SwapLegData& a, const std::string& nameA, const SwapLegData& b, const std::string& nameB);

/// 400 unless both legs are in one currency (single-currency endpoints).
void RequireOneCurrency(
    const SwapLegData& a,
    const std::string& nameA,
    const SwapLegData& b,
    const std::string& nameB,
    const std::string& endpoint);

/// Serialize one leg result (with its flows when `includeFlows`).
flatbuffers::Offset<quantra::SwapLegResult> SerializeSwapLegResult(
    flatbuffers::grpc::MessageBuilder& builder, const SwapLegPerLeg& leg, bool includeFlows);

/// Serialize the `legs` vector of a swap response.
flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<quantra::SwapLegResult>>>
SerializeSwapLegResults(
    flatbuffers::grpc::MessageBuilder& builder,
    const std::vector<SwapLegPerLeg>& legs,
    bool includeFlows);

} // namespace quantra

#endif // QUANTRA_SWAP_LEG_MAPPER_H
