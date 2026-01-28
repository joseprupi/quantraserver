#ifndef QUANTRASERVER_CAP_FLOOR_HANDLER_H
#define QUANTRASERVER_CAP_FLOOR_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "cap_floor_pricing_request.h"
#include "price_cap_floor_request_generated.h"
#include "cap_floor_response_generated.h"

using quantra::PriceCapFloorRequest;
using quantra::PriceCapFloorResponse;
using quantra::PriceCapFloorResponseBuilder;

/**
 * PriceCapFloorData - Async handler for Cap/Floor pricing.
 */
class PriceCapFloorData : public CallDataGeneric<
    PriceCapFloorRequest,
    CapFloorPricingRequest,
    PriceCapFloorResponse,
    PriceCapFloorResponseBuilder>
{
public:
    PriceCapFloorData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceCapFloor(
            &ctx_, &request_msg, &responder_, 
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceCapFloorData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceCapFloor, PriceCapFloorData);

#endif // QUANTRASERVER_CAP_FLOOR_HANDLER_H
