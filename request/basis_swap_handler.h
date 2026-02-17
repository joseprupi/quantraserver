#ifndef QUANTRASERVER_BASIS_SWAP_HANDLER_H
#define QUANTRASERVER_BASIS_SWAP_HANDLER_H

#include "basis_swap_pricing_request.h"
#include "basis_swap_response_generated.h"
#include "call_data_base.h"
#include "price_basis_swap_request_generated.h"
#include "product_registry.h"

using quantra::PriceBasisSwapRequest;
using quantra::PriceBasisSwapResponse;
using quantra::PriceBasisSwapResponseBuilder;

class PriceBasisSwapData : public CallDataGeneric<
    PriceBasisSwapRequest,
    BasisSwapPricingRequest,
    PriceBasisSwapResponse,
    PriceBasisSwapResponseBuilder> {
public:
    PriceBasisSwapData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceBasisSwap(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceBasisSwapData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(PriceBasisSwap, PriceBasisSwapData);

#endif // QUANTRASERVER_BASIS_SWAP_HANDLER_H
