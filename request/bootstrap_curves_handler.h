#ifndef QUANTRASERVER_BOOTSTRAP_CURVES_HANDLER_H
#define QUANTRASERVER_BOOTSTRAP_CURVES_HANDLER_H

#include "bootstrap_curves_mapper.h"
#include "bootstrap_curves_pricer.h"
#include "bootstrap_curves_request_generated.h"
#include "bootstrap_curves_response_generated.h"
#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"

using quantra::BootstrapCurvesRequest;
using quantra::BootstrapCurvesResponse;
using quantra::BootstrapCurvesResponseBuilder;

/// Generic endpoint binding for the BootstrapCurves query. The
/// FlatBuffers↔domain↔QuantLib↔FlatBuffers glue lives in ProductEndpoint;
/// PricingRegistryBuilder transparently bootstraps the requested curves and
/// the pricer just samples them on the requested grid.
using BootstrapCurvesEndpoint = quantra::ProductEndpoint<
    BootstrapCurvesRequest,
    BootstrapCurvesResponse,
    quantra::BootstrapCurvesMapper,
    quantra::BootstrapCurvesPricer>;

/// Transitional alias: the C++ parity test instantiates this name directly.
using BootstrapCurvesRequestHandler = BootstrapCurvesEndpoint;

/**
 * BootstrapCurvesData - Async handler for curve bootstrapping.
 *
 * Bootstraps multiple yield curves and returns requested measures
 * (discount factors, zero rates, forward rates) sampled on a grid.
 */
class BootstrapCurvesData : public CallDataGeneric<
    BootstrapCurvesRequest,
    BootstrapCurvesEndpoint,
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
