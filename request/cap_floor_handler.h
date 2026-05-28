#ifndef QUANTRASERVER_CAP_FLOOR_HANDLER_H
#define QUANTRASERVER_CAP_FLOOR_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "cap_floor_mapper.h"
#include "cap_floor_pricer.h"
#include "price_cap_floor_request_generated.h"
#include "cap_floor_response_generated.h"

using quantra::PriceCapFloorRequest;
using quantra::PriceCapFloorResponse;
using quantra::PriceCapFloorResponseBuilder;

/// Generic endpoint binding for the CapFloor product.
using CapFloorEndpoint = quantra::ProductEndpoint<
    PriceCapFloorRequest,
    PriceCapFloorResponse,
    quantra::CapFloorMapper,
    quantra::CapFloorPricer>;

/// Transitional alias for any caller that still names the legacy type.
using CapFloorPricingRequest = CapFloorEndpoint;

/**
 * PriceCapFloorData - Async handler for Cap/Floor pricing.
 */
class PriceCapFloorData : public CallDataGeneric<
    PriceCapFloorRequest,
    CapFloorEndpoint,
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
