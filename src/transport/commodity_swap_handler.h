#ifndef QUANTRASERVER_COMMODITY_SWAP_HANDLER_H
#define QUANTRASERVER_COMMODITY_SWAP_HANDLER_H

#include "call_data_base.h"
#include "commodity_swap_evaluator.h"
#include "commodity_swap_mapper.h"
#include "product_endpoint.h"
#include "product_registry.h"

#include "commodity_swap_response_generated.h"
#include "price_commodity_swap_request_generated.h"

using quantra::PriceCommoditySwapRequest;
using quantra::PriceCommoditySwapResponse;
using quantra::PriceCommoditySwapResponseBuilder;

/// Generic endpoint binding for the commodity swap product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using CommoditySwapEndpoint = quantra::ProductEndpoint<
    PriceCommoditySwapRequest,
    PriceCommoditySwapResponse,
    quantra::CommoditySwapMapper,
    quantra::CommoditySwapEvaluator>;

/**
 * PriceCommoditySwapData - Async handler for commodity swap pricing.
 */
class PriceCommoditySwapData : public CallDataGeneric<
                                   PriceCommoditySwapRequest,
                                   CommoditySwapEndpoint,
                                   PriceCommoditySwapResponse,
                                   PriceCommoditySwapResponseBuilder> {
public:
    PriceCommoditySwapData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceCommoditySwap(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceCommoditySwapData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceCommoditySwap, PriceCommoditySwapData);

#endif // QUANTRASERVER_COMMODITY_SWAP_HANDLER_H
