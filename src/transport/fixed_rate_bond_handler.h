#ifndef QUANTRASERVER_FIXED_RATE_BOND_HANDLER_H
#define QUANTRASERVER_FIXED_RATE_BOND_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "fixed_rate_bond_mapper.h"
#include "fixed_rate_bond_evaluator.h"
#include "price_fixed_rate_bond_request_generated.h"
#include "fixed_rate_bond_response_generated.h"

using quantra::PriceFixedRateBondRequest;
using quantra::PriceFixedRateBondResponse;
using quantra::PriceFixedRateBondResponseBuilder;

/// Pilot product: fixed-rate bond. The endpoint glue (Verify → mapper.toInputs →
/// registry → context → pricer.price → mapper.toResponse) lives in the generic
/// ProductEndpoint template; the handler only binds the four types and registers.
using FixedRateBondEndpoint = quantra::ProductEndpoint<
    PriceFixedRateBondRequest,
    PriceFixedRateBondResponse,
    quantra::FixedRateBondMapper,
    quantra::FixedRateBondEvaluator>;

/// Backward-compatible alias for existing call sites (tests) that referenced
/// the legacy FixedRateBondPricingRequest type directly.
using FixedRateBondPricingRequest = FixedRateBondEndpoint;

/**
 * PriceFixedRateBondData - Async gRPC handler for fixed-rate bond pricing.
 */
class PriceFixedRateBondData : public CallDataGeneric<
    PriceFixedRateBondRequest,
    FixedRateBondEndpoint,
    PriceFixedRateBondResponse,
    PriceFixedRateBondResponseBuilder>
{
public:
    PriceFixedRateBondData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceFixedRateBond(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceFixedRateBondData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceFixedRateBond, PriceFixedRateBondData);

#endif // QUANTRASERVER_FIXED_RATE_BOND_HANDLER_H
