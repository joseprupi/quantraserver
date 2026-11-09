#ifndef QUANTRASERVER_VALIDATE_VOL_SURFACE_HANDLER_H
#define QUANTRASERVER_VALIDATE_VOL_SURFACE_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "vol_surface_validation_evaluator.h"
#include "vol_surface_validation_mapper.h"

#include "validate_vol_surface_request_generated.h"
#include "validate_vol_surface_response_generated.h"

using quantra::ValidateVolSurfaceRequest;
using quantra::ValidateVolSurfaceResponse;
using quantra::ValidateVolSurfaceResponseBuilder;

/// Generic endpoint binding for the ValidateVolSurface request: the mapper
/// decodes the quote sheet, the checks and the Monte Carlo settings; the
/// evaluator tests the registry's surface against the quotes.
using ValidateVolSurfaceEndpoint = quantra::ProductEndpoint<
    ValidateVolSurfaceRequest,
    ValidateVolSurfaceResponse,
    quantra::VolSurfaceValidationMapper,
    quantra::VolSurfaceValidationEvaluator>;

class ValidateVolSurfaceData : public CallDataGeneric<
                                   ValidateVolSurfaceRequest,
                                   ValidateVolSurfaceEndpoint,
                                   ValidateVolSurfaceResponse,
                                   ValidateVolSurfaceResponseBuilder> {
public:
    ValidateVolSurfaceData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestValidateVolSurface(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new ValidateVolSurfaceData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(ValidateVolSurface, ValidateVolSurfaceData);

#endif // QUANTRASERVER_VALIDATE_VOL_SURFACE_HANDLER_H
