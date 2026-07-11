#ifndef QUANTRASERVER_ZERO_COUPON_BOND_HANDLER_H
#define QUANTRASERVER_ZERO_COUPON_BOND_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "zero_coupon_bond_mapper.h"
#include "zero_coupon_bond_evaluator.h"
#include "price_zero_coupon_bond_request_generated.h"
#include "zero_coupon_bond_response_generated.h"

using quantra::PriceZeroCouponBondRequest;
using quantra::PriceZeroCouponBondResponse;
using quantra::PriceZeroCouponBondResponseBuilder;

/// Generic endpoint binding for the zero-coupon bond product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using ZeroCouponBondEndpoint = quantra::ProductEndpoint<
    PriceZeroCouponBondRequest,
    PriceZeroCouponBondResponse,
    quantra::ZeroCouponBondMapper,
    quantra::ZeroCouponBondEvaluator>;

/**
 * PriceZeroCouponBondData - Async handler for zero-coupon bond pricing.
 */
class PriceZeroCouponBondData : public CallDataGeneric<
    PriceZeroCouponBondRequest,
    ZeroCouponBondEndpoint,
    PriceZeroCouponBondResponse,
    PriceZeroCouponBondResponseBuilder>
{
public:
    PriceZeroCouponBondData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceZeroCouponBond(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceZeroCouponBondData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceZeroCouponBond, PriceZeroCouponBondData);

#endif // QUANTRASERVER_ZERO_COUPON_BOND_HANDLER_H
