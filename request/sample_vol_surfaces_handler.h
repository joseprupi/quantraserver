#ifndef QUANTRASERVER_SAMPLE_VOL_SURFACES_HANDLER_H
#define QUANTRASERVER_SAMPLE_VOL_SURFACES_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "sample_vol_surfaces_request.h"
#include "sample_vol_surfaces_request_generated.h"
#include "sample_vol_surfaces_response_generated.h"

using quantra::SampleVolSurfacesRequest;
using quantra::SampleVolSurfacesResponse;
using quantra::SampleVolSurfacesResponseBuilder;

class SampleVolSurfacesData : public CallDataGeneric<
    SampleVolSurfacesRequest,
    SampleVolSurfacesRequestHandler,
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
