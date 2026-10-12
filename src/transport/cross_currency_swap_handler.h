#ifndef QUANTRASERVER_CROSS_CURRENCY_SWAP_HANDLER_H
#define QUANTRASERVER_CROSS_CURRENCY_SWAP_HANDLER_H

#include "call_data_base.h"
#include "cross_currency_swap_evaluator.h"
#include "cross_currency_swap_mapper.h"
#include "product_endpoint.h"
#include "product_registry.h"

#include "cross_currency_swap_response_generated.h"
#include "price_cross_currency_swap_request_generated.h"

using quantra::PriceCrossCurrencySwapRequest;
using quantra::PriceCrossCurrencySwapResponse;
using quantra::PriceCrossCurrencySwapResponseBuilder;

/// Generic endpoint binding for the cross-currency swap product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using CrossCurrencySwapEndpoint = quantra::ProductEndpoint<
    PriceCrossCurrencySwapRequest,
    PriceCrossCurrencySwapResponse,
    quantra::CrossCurrencySwapMapper,
    quantra::CrossCurrencySwapEvaluator>;

/**
 * PriceCrossCurrencySwapData - Async handler for cross-currency swap pricing.
 */
class PriceCrossCurrencySwapData : public CallDataGeneric<
                                       PriceCrossCurrencySwapRequest,
                                       CrossCurrencySwapEndpoint,
                                       PriceCrossCurrencySwapResponse,
                                       PriceCrossCurrencySwapResponseBuilder> {
public:
    PriceCrossCurrencySwapData(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceCrossCurrencySwap(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceCrossCurrencySwapData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceCrossCurrencySwap, PriceCrossCurrencySwapData);

#endif // QUANTRASERVER_CROSS_CURRENCY_SWAP_HANDLER_H
