#ifndef QUANTRASERVER_OIS_SWAP_HANDLER_H
#define QUANTRASERVER_OIS_SWAP_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "ois_swap_mapper.h"
#include "ois_swap_pricer.h"
#include "price_ois_swap_request_generated.h"
#include "ois_swap_response_generated.h"

using quantra::PriceOisSwapRequest;
using quantra::PriceOisSwapResponse;
using quantra::PriceOisSwapResponseBuilder;

/// Generic endpoint binding for the OIS swap product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using OisSwapEndpoint = quantra::ProductEndpoint<
    PriceOisSwapRequest,
    PriceOisSwapResponse,
    quantra::OisSwapMapper,
    quantra::OisSwapPricer>;

/// Transitional alias for any caller that still names the legacy type.
using OisSwapPricingRequest = OisSwapEndpoint;

/**
 * PriceOisSwapData - Async gRPC handler for OIS swap pricing.
 */
class PriceOisSwapData : public CallDataGeneric<
    PriceOisSwapRequest,
    OisSwapEndpoint,
    PriceOisSwapResponse,
    PriceOisSwapResponseBuilder>
{
public:
    PriceOisSwapData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceOisSwap(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceOisSwapData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceOisSwap, PriceOisSwapData);

#endif // QUANTRASERVER_OIS_SWAP_HANDLER_H
