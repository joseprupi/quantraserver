#ifndef QUANTRASERVER_ZERO_COUPON_INFLATION_SWAP_PRICING_REQUEST_H
#define QUANTRASERVER_ZERO_COUPON_INFLATION_SWAP_PRICING_REQUEST_H

#include "flatbuffers/grpc.h"
#include "quantra_request.h"
#include "price_zero_coupon_inflation_swap_request_generated.h"
#include "zero_coupon_inflation_swap_response_generated.h"

class ZeroCouponInflationSwapPricingRequest
    : QuantraRequest<quantra::PriceZeroCouponInflationSwapRequest,
                     quantra::PriceZeroCouponInflationSwapResponse> {
public:
    flatbuffers::Offset<quantra::PriceZeroCouponInflationSwapResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceZeroCouponInflationSwapRequest* request) const;
};

#endif // QUANTRASERVER_ZERO_COUPON_INFLATION_SWAP_PRICING_REQUEST_H
