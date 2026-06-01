#ifndef QUANTRASERVER_ZERO_COUPON_INFLATION_SWAP_HANDLER_H
#define QUANTRASERVER_ZERO_COUPON_INFLATION_SWAP_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "zero_coupon_inflation_swap_mapper.h"
#include "zero_coupon_inflation_swap_evaluator.h"
#include "price_zero_coupon_inflation_swap_request_generated.h"
#include "zero_coupon_inflation_swap_response_generated.h"

using quantra::PriceZeroCouponInflationSwapRequest;
using quantra::PriceZeroCouponInflationSwapResponse;
using quantra::PriceZeroCouponInflationSwapResponseBuilder;

/// Wave-A product: zero-coupon inflation swap (ZCIIS). Mirrors the VanillaSwap
/// cutover — the generic ProductEndpoint template owns the
/// Verify → mapper.toInputs → registry → context → pricer.price →
/// mapper.toResponse glue.
using ZeroCouponInflationSwapEndpoint = quantra::ProductEndpoint<
    PriceZeroCouponInflationSwapRequest,
    PriceZeroCouponInflationSwapResponse,
    quantra::ZeroCouponInflationSwapMapper,
    quantra::ZeroCouponInflationSwapEvaluator>;

/// Backward-compatible alias for existing call sites (tests) that referenced
/// the legacy ZeroCouponInflationSwapPricingRequest type directly.
using ZeroCouponInflationSwapPricingRequest = ZeroCouponInflationSwapEndpoint;

class PriceZeroCouponInflationSwapData : public CallDataGeneric<
    PriceZeroCouponInflationSwapRequest,
    ZeroCouponInflationSwapEndpoint,
    PriceZeroCouponInflationSwapResponse,
    PriceZeroCouponInflationSwapResponseBuilder> {
public:
    PriceZeroCouponInflationSwapData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceZeroCouponInflationSwap(
            &ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceZeroCouponInflationSwapData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(PriceZeroCouponInflationSwap, PriceZeroCouponInflationSwapData);

#endif // QUANTRASERVER_ZERO_COUPON_INFLATION_SWAP_HANDLER_H
