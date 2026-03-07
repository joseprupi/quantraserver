#ifndef QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_HANDLER_H
#define QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "bootstrap_inflation_curves_request.h"
#include "bootstrap_inflation_curves_request_generated.h"
#include "bootstrap_inflation_curves_response_generated.h"

using quantra::BootstrapInflationCurvesRequest;
using quantra::BootstrapInflationCurvesResponse;
using quantra::BootstrapInflationCurvesResponseBuilder;

/**
 * BootstrapInflationCurvesData - Async handler for inflation curve bootstrapping.
 */
class BootstrapInflationCurvesData : public CallDataGeneric<
    BootstrapInflationCurvesRequest,
    BootstrapInflationCurvesRequestHandler,
    BootstrapInflationCurvesResponse,
    BootstrapInflationCurvesResponseBuilder>
{
public:
    BootstrapInflationCurvesData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestBootstrapInflationCurves(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new BootstrapInflationCurvesData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(BootstrapInflationCurves, BootstrapInflationCurvesData);

#endif // QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_HANDLER_H

