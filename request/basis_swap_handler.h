#ifndef QUANTRASERVER_BASIS_SWAP_HANDLER_H
#define QUANTRASERVER_BASIS_SWAP_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "basis_swap_mapper.h"
#include "basis_swap_pricer.h"
#include "price_basis_swap_request_generated.h"
#include "basis_swap_response_generated.h"

using quantra::PriceBasisSwapRequest;
using quantra::PriceBasisSwapResponse;
using quantra::PriceBasisSwapResponseBuilder;

/// Generic endpoint binding for the basis swap product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using BasisSwapEndpoint = quantra::ProductEndpoint<
    PriceBasisSwapRequest,
    PriceBasisSwapResponse,
    quantra::BasisSwapMapper,
    quantra::BasisSwapPricer>;

/// Transitional alias for any caller that still names the legacy type.
using BasisSwapPricingRequest = BasisSwapEndpoint;

/**
 * PriceBasisSwapData - Async gRPC handler for basis swap pricing.
 */
class PriceBasisSwapData : public CallDataGeneric<
    PriceBasisSwapRequest,
    BasisSwapEndpoint,
    PriceBasisSwapResponse,
    PriceBasisSwapResponseBuilder>
{
public:
    PriceBasisSwapData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceBasisSwap(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceBasisSwapData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceBasisSwap, PriceBasisSwapData);

#endif // QUANTRASERVER_BASIS_SWAP_HANDLER_H
