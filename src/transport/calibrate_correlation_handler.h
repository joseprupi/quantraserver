#ifndef QUANTRASERVER_CALIBRATE_CORRELATION_HANDLER_H
#define QUANTRASERVER_CALIBRATE_CORRELATION_HANDLER_H

#include "call_data_base.h"
#include "correlation_calibration_evaluator.h"
#include "correlation_calibration_mapper.h"
#include "product_endpoint.h"
#include "product_registry.h"

#include "calibrate_correlation_request_generated.h"
#include "calibrate_correlation_response_generated.h"

using quantra::CalibrateCorrelationRequest;
using quantra::CalibrateCorrelationResponse;
using quantra::CalibrateCorrelationResponseBuilder;

/// Generic endpoint binding for the CalibrateCorrelation request: no
/// Pricing block (ProductEndpoint elides the registry), the mapper decodes
/// the price series and the evaluator aligns them and estimates the matrix.
using CalibrateCorrelationEndpoint = quantra::ProductEndpoint<
    CalibrateCorrelationRequest,
    CalibrateCorrelationResponse,
    quantra::CorrelationCalibrationMapper,
    quantra::CorrelationCalibrationEvaluator>;

class CalibrateCorrelationData : public CallDataGeneric<
                                     CalibrateCorrelationRequest,
                                     CalibrateCorrelationEndpoint,
                                     CalibrateCorrelationResponse,
                                     CalibrateCorrelationResponseBuilder> {
public:
    CalibrateCorrelationData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestCalibrateCorrelation(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalibrateCorrelationData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalibrateCorrelation, CalibrateCorrelationData);

#endif // QUANTRASERVER_CALIBRATE_CORRELATION_HANDLER_H
