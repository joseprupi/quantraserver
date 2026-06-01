#ifndef QUANTRASERVER_FRA_HANDLER_H
#define QUANTRASERVER_FRA_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "fra_mapper.h"
#include "fra_evaluator.h"
#include "price_fra_request_generated.h"
#include "fra_response_generated.h"

using quantra::PriceFRARequest;
using quantra::PriceFRAResponse;
using quantra::PriceFRAResponseBuilder;

/// Generic endpoint binding for the FRA product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using FraEndpoint = quantra::ProductEndpoint<
    PriceFRARequest,
    PriceFRAResponse,
    quantra::FraMapper,
    quantra::FraEvaluator>;

/// Transitional alias for any caller that still names the legacy type.
using FRAPricingRequest = FraEndpoint;

/**
 * PriceFRAData - Async handler for FRA pricing.
 */
class PriceFRAData : public CallDataGeneric<
    PriceFRARequest,
    FraEndpoint,
    PriceFRAResponse,
    PriceFRAResponseBuilder>
{
public:
    PriceFRAData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceFRA(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceFRAData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceFRA, PriceFRAData);

#endif // QUANTRASERVER_FRA_HANDLER_H
