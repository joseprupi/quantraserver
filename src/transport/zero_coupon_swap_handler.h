#ifndef QUANTRASERVER_ZERO_COUPON_SWAP_HANDLER_H
#define QUANTRASERVER_ZERO_COUPON_SWAP_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "zero_coupon_swap_mapper.h"
#include "zero_coupon_swap_evaluator.h"
#include "price_zero_coupon_swap_request_generated.h"
#include "zero_coupon_swap_response_generated.h"

using quantra::PriceZeroCouponSwapRequest;
using quantra::PriceZeroCouponSwapResponse;
using quantra::PriceZeroCouponSwapResponseBuilder;

/// Generic endpoint binding for the zero-coupon swap product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using ZeroCouponSwapEndpoint = quantra::ProductEndpoint<
    PriceZeroCouponSwapRequest,
    PriceZeroCouponSwapResponse,
    quantra::ZeroCouponSwapMapper,
    quantra::ZeroCouponSwapEvaluator>;

/**
 * PriceZeroCouponSwapData - Async handler for zero-coupon swap pricing.
 */
class PriceZeroCouponSwapData : public CallDataGeneric<
    PriceZeroCouponSwapRequest,
    ZeroCouponSwapEndpoint,
    PriceZeroCouponSwapResponse,
    PriceZeroCouponSwapResponseBuilder>
{
public:
    PriceZeroCouponSwapData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceZeroCouponSwap(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceZeroCouponSwapData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceZeroCouponSwap, PriceZeroCouponSwapData);

#endif // QUANTRASERVER_ZERO_COUPON_SWAP_HANDLER_H
