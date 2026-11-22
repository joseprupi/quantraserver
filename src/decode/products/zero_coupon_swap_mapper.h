#ifndef QUANTRA_ZERO_COUPON_SWAP_MAPPER_H
#define QUANTRA_ZERO_COUPON_SWAP_MAPPER_H

/**
 * ZeroCouponSwapMapper — the one place zero-coupon-swap flatbuffers live.
 * Decodes a PriceZeroCouponSwapRequest into plain ZeroCouponSwapInputs and
 * serializes the ZeroCouponSwapResult back into a PriceZeroCouponSwapResponse.
 */

#include "zero_coupon_swap_evaluator.h"

#include "price_zero_coupon_swap_request_generated.h"
#include "zero_coupon_swap_response_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class ZeroCouponSwapMapper {
public:
    ZeroCouponSwapInputs toInputs(const quantra::PriceZeroCouponSwapRequest* req) const;

    flatbuffers::Offset<quantra::PriceZeroCouponSwapResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const ZeroCouponSwapResult& result) const;

    /// The response of a `price_only` request: one NPV per trade
    /// (price_only_response.h).
    flatbuffers::Offset<quantra::PriceZeroCouponSwapResponse> toPriceOnlyResponse(
        flatbuffers::grpc::MessageBuilder& builder, const ZeroCouponSwapResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_ZERO_COUPON_SWAP_MAPPER_H
