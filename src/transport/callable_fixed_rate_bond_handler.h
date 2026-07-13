#ifndef QUANTRASERVER_CALLABLE_FIXED_RATE_BOND_HANDLER_H
#define QUANTRASERVER_CALLABLE_FIXED_RATE_BOND_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "callable_fixed_rate_bond_mapper.h"
#include "callable_fixed_rate_bond_evaluator.h"
#include "price_callable_fixed_rate_bond_request_generated.h"
#include "callable_fixed_rate_bond_response_generated.h"

using quantra::PriceCallableFixedRateBondRequest;
using quantra::PriceCallableFixedRateBondResponse;
using quantra::PriceCallableFixedRateBondResponseBuilder;

/// Callable/puttable fixed-rate bond endpoint. The generic ProductEndpoint
/// template owns the glue (Verify -> mapper.toInputs -> registry -> context ->
/// evaluator.evaluate -> mapper.toResponse); the handler only binds the types.
using CallableFixedRateBondEndpoint = quantra::ProductEndpoint<
    PriceCallableFixedRateBondRequest,
    PriceCallableFixedRateBondResponse,
    quantra::CallableFixedRateBondMapper,
    quantra::CallableFixedRateBondEvaluator>;

/**
 * PriceCallableFixedRateBondData - Async gRPC handler for callable fixed-rate
 * bond pricing.
 */
class PriceCallableFixedRateBondData : public CallDataGeneric<
    PriceCallableFixedRateBondRequest,
    CallableFixedRateBondEndpoint,
    PriceCallableFixedRateBondResponse,
    PriceCallableFixedRateBondResponseBuilder>
{
public:
    PriceCallableFixedRateBondData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceCallableFixedRateBond(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceCallableFixedRateBondData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceCallableFixedRateBond, PriceCallableFixedRateBondData);

#endif // QUANTRASERVER_CALLABLE_FIXED_RATE_BOND_HANDLER_H
