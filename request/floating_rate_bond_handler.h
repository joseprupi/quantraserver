#ifndef QUANTRASERVER_FLOATING_RATE_BOND_HANDLER_H
#define QUANTRASERVER_FLOATING_RATE_BOND_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "floating_rate_bond_mapper.h"
#include "floating_rate_bond_pricer.h"
#include "price_floating_rate_bond_request_generated.h"
#include "floating_rate_bond_response_generated.h"

using quantra::PriceFloatingRateBondRequest;
using quantra::PriceFloatingRateBondResponse;
using quantra::PriceFloatingRateBondResponseBuilder;

/// Generic endpoint binding for the floating-rate bond product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using FloatingRateBondEndpoint = quantra::ProductEndpoint<
    PriceFloatingRateBondRequest,
    PriceFloatingRateBondResponse,
    quantra::FloatingRateBondMapper,
    quantra::FloatingRateBondPricer>;

/// Transitional alias for any caller that still names the legacy type.
using FloatingRateBondPricingRequest = FloatingRateBondEndpoint;

/**
 * PriceFloatingRateBondData - Async gRPC handler for floating-rate bond pricing.
 */
class PriceFloatingRateBondData : public CallDataGeneric<
    PriceFloatingRateBondRequest,
    FloatingRateBondEndpoint,
    PriceFloatingRateBondResponse,
    PriceFloatingRateBondResponseBuilder>
{
public:
    PriceFloatingRateBondData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceFloatingRateBond(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceFloatingRateBondData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceFloatingRateBond, PriceFloatingRateBondData);

#endif // QUANTRASERVER_FLOATING_RATE_BOND_HANDLER_H
