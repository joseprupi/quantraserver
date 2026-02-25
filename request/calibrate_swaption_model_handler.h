#ifndef QUANTRASERVER_CALIBRATE_SWAPTION_MODEL_HANDLER_H
#define QUANTRASERVER_CALIBRATE_SWAPTION_MODEL_HANDLER_H

#include "calibrate_swaption_model_pricing_request.h"
#include "call_data_base.h"
#include "product_registry.h"

#include "calibrate_swaption_model_request_generated.h"
#include "calibrate_swaption_model_response_generated.h"

using quantra::CalibrateSwaptionModelRequest;
using quantra::CalibrateSwaptionModelResponse;
using quantra::CalibrateSwaptionModelResponseBuilder;

class CalibrateSwaptionModelData : public CallDataGeneric<
    CalibrateSwaptionModelRequest,
    CalibrateSwaptionModelPricingRequest,
    CalibrateSwaptionModelResponse,
    CalibrateSwaptionModelResponseBuilder> {
public:
    CalibrateSwaptionModelData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestCalibrateSwaptionModel(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalibrateSwaptionModelData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalibrateSwaptionModel, CalibrateSwaptionModelData);

#endif // QUANTRASERVER_CALIBRATE_SWAPTION_MODEL_HANDLER_H
