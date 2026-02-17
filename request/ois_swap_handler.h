#ifndef QUANTRASERVER_OIS_SWAP_HANDLER_H
#define QUANTRASERVER_OIS_SWAP_HANDLER_H

#include "call_data_base.h"
#include "ois_swap_pricing_request.h"
#include "price_ois_swap_request_generated.h"
#include "ois_swap_response_generated.h"
#include "product_registry.h"

using quantra::PriceOisSwapRequest;
using quantra::PriceOisSwapResponse;
using quantra::PriceOisSwapResponseBuilder;

class PriceOisSwapData : public CallDataGeneric<
    PriceOisSwapRequest,
    OisSwapPricingRequest,
    PriceOisSwapResponse,
    PriceOisSwapResponseBuilder> {
public:
    PriceOisSwapData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceOisSwap(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceOisSwapData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(PriceOisSwap, PriceOisSwapData);

#endif // QUANTRASERVER_OIS_SWAP_HANDLER_H
