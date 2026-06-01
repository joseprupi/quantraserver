#ifndef QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_HANDLER_H
#define QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_HANDLER_H

#include <exception>
#include <memory>
#include <string>

#include "bootstrap_inflation_curves_mapper.h"
#include "bootstrap_inflation_curves_evaluator.h"
#include "bootstrap_inflation_curves_request_generated.h"
#include "bootstrap_inflation_curves_response_generated.h"
#include "call_data_base.h"
#include "eval_date_guard.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "quantra_request.h"

using quantra::BootstrapInflationCurvesRequest;
using quantra::BootstrapInflationCurvesResponse;
using quantra::BootstrapInflationCurvesResponseBuilder;

namespace quantra {

/// Endpoint binding for the BootstrapInflationCurves query. This is a
/// list/query endpoint: it uses the generic ProductEndpoint glue and opts into
/// the registry-build-error policy via BootstrapInflationCurvesMapper's
/// onRegistryBuildError hook, so a build failure is reported as per-curve Error
/// entries (HTTP 200 list) rather than as a transport-level error.
using BootstrapInflationCurvesEndpoint = ProductEndpoint<
    BootstrapInflationCurvesRequest,
    BootstrapInflationCurvesResponse,
    BootstrapInflationCurvesMapper,
    BootstrapInflationCurvesEvaluator>;

} // namespace quantra

/// Transitional alias: the C++ parity test instantiates this name directly.
using BootstrapInflationCurvesRequestHandler = quantra::BootstrapInflationCurvesEndpoint;

/**
 * BootstrapInflationCurvesData - Async handler for inflation curve bootstrapping.
 */
class BootstrapInflationCurvesData : public CallDataGeneric<
    BootstrapInflationCurvesRequest,
    quantra::BootstrapInflationCurvesEndpoint,
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
