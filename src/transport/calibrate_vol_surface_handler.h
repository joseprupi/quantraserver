#ifndef QUANTRASERVER_CALIBRATE_VOL_SURFACE_HANDLER_H
#define QUANTRASERVER_CALIBRATE_VOL_SURFACE_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "vol_surface_calibration_evaluator.h"
#include "vol_surface_calibration_mapper.h"

#include "calibrate_vol_surface_request_generated.h"
#include "calibrate_vol_surface_response_generated.h"

using quantra::CalibrateVolSurfaceRequest;
using quantra::CalibrateVolSurfaceResponse;
using quantra::CalibrateVolSurfaceResponseBuilder;

/// Generic endpoint binding for the CalibrateVolSurface request: the mapper
/// decodes the quote sheet, forwards and grid; the evaluator fits the
/// surface (Andreasen-Huge or SVI per expiry) against the registry's spot
/// and discounting curve.
using CalibrateVolSurfaceEndpoint = quantra::ProductEndpoint<
    CalibrateVolSurfaceRequest,
    CalibrateVolSurfaceResponse,
    quantra::VolSurfaceCalibrationMapper,
    quantra::VolSurfaceCalibrationEvaluator>;

class CalibrateVolSurfaceData : public CallDataGeneric<
                                    CalibrateVolSurfaceRequest,
                                    CalibrateVolSurfaceEndpoint,
                                    CalibrateVolSurfaceResponse,
                                    CalibrateVolSurfaceResponseBuilder> {
public:
    CalibrateVolSurfaceData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestCalibrateVolSurface(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalibrateVolSurfaceData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalibrateVolSurface, CalibrateVolSurfaceData);

#endif // QUANTRASERVER_CALIBRATE_VOL_SURFACE_HANDLER_H
