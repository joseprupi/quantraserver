#ifndef QUANTRASERVER_CALIBRATE_SWAPTION_VOL_HANDLER_H
#define QUANTRASERVER_CALIBRATE_SWAPTION_VOL_HANDLER_H

#include "calibrate_swaption_vol_mapper.h"
#include "calibrate_swaption_vol_pricer.h"
#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"

#include "calibrate_swaption_vol_request_generated.h"
#include "calibrate_swaption_vol_response_generated.h"

using quantra::CalibrateSwaptionVolRequest;
using quantra::CalibrateSwaptionVolResponse;
using quantra::CalibrateSwaptionVolResponseBuilder;

/// Generic endpoint binding for the CalibrateSwaptionVol request. The
/// FlatBuffers↔domain↔QuantLib↔FlatBuffers glue lives in ProductEndpoint; the
/// mapper extracts the surface/curve ids and the pricer delegates to the
/// shared finalizeSwaptionVolEntryForPricing helper.
using CalibrateSwaptionVolEndpoint = quantra::ProductEndpoint<
    CalibrateSwaptionVolRequest,
    CalibrateSwaptionVolResponse,
    quantra::CalibrateSwaptionVolMapper,
    quantra::CalibrateSwaptionVolPricer>;

/// Transitional alias: the C++ parity test instantiates this name directly.
using CalibrateSwaptionVolPricingRequest = CalibrateSwaptionVolEndpoint;

class CalibrateSwaptionVolData : public CallDataGeneric<
    CalibrateSwaptionVolRequest,
    CalibrateSwaptionVolEndpoint,
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
