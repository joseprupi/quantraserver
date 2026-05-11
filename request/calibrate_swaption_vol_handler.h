#ifndef QUANTRASERVER_CALIBRATE_SWAPTION_VOL_HANDLER_H
#define QUANTRASERVER_CALIBRATE_SWAPTION_VOL_HANDLER_H

#include "calibrate_swaption_vol_pricing_request.h"
#include "call_data_base.h"
#include "product_registry.h"

#include "calibrate_swaption_vol_request_generated.h"
#include "calibrate_swaption_vol_response_generated.h"

using quantra::CalibrateSwaptionVolRequest;
using quantra::CalibrateSwaptionVolResponse;
using quantra::CalibrateSwaptionVolResponseBuilder;

class CalibrateSwaptionVolData : public CallDataGeneric<
    CalibrateSwaptionVolRequest,
    CalibrateSwaptionVolPricingRequest,
    CalibrateSwaptionVolResponse,
    CalibrateSwaptionVolResponseBuilder> {
public:
    CalibrateSwaptionVolData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestCalibrateSwaptionVol(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalibrateSwaptionVolData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalibrateSwaptionVol, CalibrateSwaptionVolData);

#endif // QUANTRASERVER_CALIBRATE_SWAPTION_VOL_HANDLER_H
