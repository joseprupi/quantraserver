#ifndef QUANTRASERVER_FIXED_RATE_BOND_HANDLER_H
#define QUANTRASERVER_FIXED_RATE_BOND_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "fixed_rate_bond_pricing_request.h"
#include "price_fixed_rate_bond_request_generated.h"
#include "fixed_rate_bond_response_generated.h"

using quantra::PriceFixedRateBondRequest;
using quantra::PriceFixedRateBondResponse;
using quantra::PriceFixedRateBondResponseBuilder;

/**
 * PriceFixedRateBondData - Async handler for fixed rate bond pricing.
 */
class PriceFixedRateBondData : public CallDataGeneric<
    PriceFixedRateBondRequest,
    FixedRateBondPricingRequest,
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