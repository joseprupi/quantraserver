#ifndef QUANTRA_ZERO_COUPON_INFLATION_SWAP_MAPPER_H
#define QUANTRA_ZERO_COUPON_INFLATION_SWAP_MAPPER_H

/**
 * ZeroCouponInflationSwapMapper — the one place ZCIIS flatbuffers live.
 * Decodes the PriceZeroCouponInflationSwapRequest into plain
 * ZeroCouponInflationSwapInputs and serializes the
 * ZeroCouponInflationSwapResult back into a PriceZeroCouponInflationSwapResponse.
 */

#include "flatbuffers/grpc.h"

#include "price_zero_coupon_inflation_swap_request_generated.h"
#include "zero_coupon_inflation_swap_pricer.h"
#include "zero_coupon_inflation_swap_response_generated.h"

namespace quantra {

class ZeroCouponInflationSwapMapper {
public:
    ZeroCouponInflationSwapInputs toInputs(
        const quantra::PriceZeroCouponInflationSwapRequest* req) const;

    flatbuffers::Offset<quantra::PriceZeroCouponInflationSwapResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const ZeroCouponInflationSwapResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_ZERO_COUPON_INFLATION_SWAP_MAPPER_H
