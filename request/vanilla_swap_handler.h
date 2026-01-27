#ifndef QUANTRASERVER_VANILLA_SWAP_HANDLER_H
#define QUANTRASERVER_VANILLA_SWAP_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "vanilla_swap_pricing_request.h"
#include "price_vanilla_swap_request_generated.h"
#include "vanilla_swap_response_generated.h"

using quantra::PriceVanillaSwapRequest;
using quantra::PriceVanillaSwapResponse;
using quantra::PriceVanillaSwapResponseBuilder;

/**
 * PriceVanillaSwapData - Async handler for vanilla swap pricing.
 */
class PriceVanillaSwapData : public CallDataGeneric<
    PriceVanillaSwapRequest,
    VanillaSwapPricingRequest,
    PriceVanillaSwapResponse,
    PriceVanillaSwapResponseBuilder>
{
public:
    PriceVanillaSwapData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceVanillaSwap(
            &ctx_, &request_msg, &responder_, 
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceVanillaSwapData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceVanillaSwap, PriceVanillaSwapData);

#endif // QUANTRASERVER_VANILLA_SWAP_HANDLER_H