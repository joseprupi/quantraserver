#ifndef QUANTRASERVER_BOOTSTRAP_CURVES_HANDLER_H
#define QUANTRASERVER_BOOTSTRAP_CURVES_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "bootstrap_curves_request.h"
#include "bootstrap_curves_request_generated.h"
#include "bootstrap_curves_response_generated.h"

using quantra::BootstrapCurvesRequest;
using quantra::BootstrapCurvesResponse;
using quantra::BootstrapCurvesResponseBuilder;

/**
 * BootstrapCurvesData - Async handler for curve bootstrapping.
 * 
 * Bootstraps multiple yield curves and returns requested measures
 * (discount factors, zero rates, forward rates) sampled on a grid.
 */
class BootstrapCurvesData : public CallDataGeneric<
    BootstrapCurvesRequest,
    BootstrapCurvesRequestHandler,
    BootstrapCurvesResponse,
    BootstrapCurvesResponseBuilder>
{
public:
    BootstrapCurvesData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestBootstrapCurves(
            &ctx_, &request_msg, &responder_, 
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new BootstrapCurvesData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(BootstrapCurves, BootstrapCurvesData);

#endif
