#ifndef QUANTRASERVER_CALIBRATE_IMPLIED_FORWARDS_HANDLER_H
#define QUANTRASERVER_CALIBRATE_IMPLIED_FORWARDS_HANDLER_H

#include "call_data_base.h"
#include "implied_forwards_evaluator.h"
#include "implied_forwards_mapper.h"
#include "product_endpoint.h"
#include "product_registry.h"

#include "calibrate_implied_forwards_request_generated.h"
#include "calibrate_implied_forwards_response_generated.h"

using quantra::CalibrateImpliedForwardsRequest;
using quantra::CalibrateImpliedForwardsResponse;
using quantra::CalibrateImpliedForwardsResponseBuilder;

/// Generic endpoint binding for the CalibrateImpliedForwards request: the
/// mapper decodes the quote sheet and the evaluator runs the per-expiry
/// put-call-parity fit against the registry's spot and discounting curve.
using CalibrateImpliedForwardsEndpoint = quantra::ProductEndpoint<
    CalibrateImpliedForwardsRequest,
    CalibrateImpliedForwardsResponse,
    quantra::ImpliedForwardsMapper,
    quantra::ImpliedForwardsEvaluator>;

class CalibrateImpliedForwardsData : public CallDataGeneric<
                                         CalibrateImpliedForwardsRequest,
                                         CalibrateImpliedForwardsEndpoint,
                                         CalibrateImpliedForwardsResponse,
                                         CalibrateImpliedForwardsResponseBuilder> {
public:
    CalibrateImpliedForwardsData(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestCalibrateImpliedForwards(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalibrateImpliedForwardsData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalibrateImpliedForwards, CalibrateImpliedForwardsData);

#endif // QUANTRASERVER_CALIBRATE_IMPLIED_FORWARDS_HANDLER_H
