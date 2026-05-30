#ifndef QUANTRASERVER_SAMPLE_VOL_SURFACES_HANDLER_H
#define QUANTRASERVER_SAMPLE_VOL_SURFACES_HANDLER_H

#include <memory>

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "sample_vol_surfaces_mapper.h"
#include "sample_vol_surfaces_pricer.h"
#include "sample_vol_surfaces_request_generated.h"
#include "sample_vol_surfaces_response_generated.h"

using quantra::SampleVolSurfacesRequest;
using quantra::SampleVolSurfacesResponse;
using quantra::SampleVolSurfacesResponseBuilder;

namespace quantra {

/// Endpoint binding for the SampleVolSurfaces query. This is a list/query
/// endpoint: it uses the generic ProductEndpoint glue and opts into the
/// registry-build-error policy via SampleVolSurfacesMapper's
/// onRegistryBuildError hook, so a build failure is reported as per-query
/// VolSurfaceSample Error entries (HTTP 200 list). A malformed top-level
/// request (toInputs throwing) propagates as a transport-level error, and
/// per-query failures during sampling are emitted as per-item errors by the
/// pricer.
using SampleVolSurfacesEndpoint = ProductEndpoint<
    SampleVolSurfacesRequest,
    SampleVolSurfacesResponse,
    SampleVolSurfacesMapper,
    SampleVolSurfacesPricer>;

} // namespace quantra

/// Transitional alias: the C++ parity test instantiates this name directly.
using SampleVolSurfacesRequestHandler = quantra::SampleVolSurfacesEndpoint;

class SampleVolSurfacesData : public CallDataGeneric<
    SampleVolSurfacesRequest,
    quantra::SampleVolSurfacesEndpoint,
    SampleVolSurfacesResponse,
    SampleVolSurfacesResponseBuilder>
{
public:
    SampleVolSurfacesData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

protected:
    void RequestCall() override
    {
        service_->RequestSampleVolSurfaces(
            &ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new SampleVolSurfacesData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(SampleVolSurfaces, SampleVolSurfacesData);

#endif // QUANTRASERVER_SAMPLE_VOL_SURFACES_HANDLER_H
