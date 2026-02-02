#ifndef QUANTRASERVER_SWAPTION_HANDLER_H
#define QUANTRASERVER_SWAPTION_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "swaption_pricing_request.h"
#include "price_swaption_request_generated.h"
#include "swaption_response_generated.h"

using quantra::PriceSwaptionRequest;
using quantra::PriceSwaptionResponse;
using quantra::PriceSwaptionResponseBuilder;

/**
 * PriceSwaptionData - Async handler for Swaption pricing.
 */
class PriceSwaptionData : public CallDataGeneric<
    PriceSwaptionRequest,
    SwaptionPricingRequest,
    PriceSwaptionResponse,
    PriceSwaptionResponseBuilder>
{
public:
    PriceSwaptionData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceSwaption(
            &ctx_, &request_msg, &responder_, 
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceSwaptionData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceSwaption, PriceSwaptionData);

#endif // QUANTRASERVER_SWAPTION_HANDLER_H
