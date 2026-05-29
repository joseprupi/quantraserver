#ifndef QUANTRASERVER_SAMPLE_VOL_SURFACES_HANDLER_H
#define QUANTRASERVER_SAMPLE_VOL_SURFACES_HANDLER_H

#include <exception>
#include <memory>
#include <string>

#include "call_data_base.h"
#include "eval_date_guard.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "product_registry.h"
#include "quantra_request.h"
#include "sample_vol_surfaces_mapper.h"
#include "sample_vol_surfaces_pricer.h"
#include "sample_vol_surfaces_request_generated.h"
#include "sample_vol_surfaces_response_generated.h"

using quantra::SampleVolSurfacesRequest;
using quantra::SampleVolSurfacesResponse;
using quantra::SampleVolSurfacesResponseBuilder;

namespace quantra {

/// Endpoint binding for the SampleVolSurfaces query. Mirrors the generic
/// ProductEndpoint glue, but reproduces the legacy top-level error shape:
/// failures during input validation or registry build are caught here and
/// embedded as per-query VolSurfaceSample error entries (rather than surfacing
/// as a transport-level error). Per-query failures during sampling are handled
/// inside the pricer.
class SampleVolSurfacesEndpoint
    : public QuantraRequest<SampleVolSurfacesRequest, SampleVolSurfacesResponse> {
public:
    flatbuffers::Offset<SampleVolSurfacesResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const SampleVolSurfacesRequest* req) const override
    {
        EvalDateGuard guard;
        try {
            auto inputs = mapper_.toInputs(req);
            PricingRegistry reg = PricingRegistryBuilder{}.build(req->pricing());
            PricingContext ctx = makeContext(req->pricing(), reg);
            auto result = pricer_.price(inputs, reg, ctx);
            return mapper_.toResponse(*builder, result);
        } catch (const std::exception& e) {
            return mapper_.toErrorResponse(*builder, req, e.what());
        }
    }

private:
    SampleVolSurfacesMapper mapper_;
    SampleVolSurfacesPricer pricer_;
};

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
