#ifndef QUANTRASERVER_CALIBRATE_EQUITY_VOL_HANDLER_H
#define QUANTRASERVER_CALIBRATE_EQUITY_VOL_HANDLER_H

#include "calibrate_equity_vol_evaluator.h"
#include "calibrate_equity_vol_mapper.h"
#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"

#include "calibrate_equity_vol_request_generated.h"
#include "calibrate_equity_vol_response_generated.h"

using quantra::CalibrateEquityVolRequest;
using quantra::CalibrateEquityVolResponse;
using quantra::CalibrateEquityVolResponseBuilder;

/// Generic endpoint binding for the CalibrateEquityVol request. The FlatBuffers glue lives in
/// ProductEndpoint; the mapper extracts the surface id, the evaluator returns the SSVI fit that
/// the registry already computed when it parsed the SsviCalibrate surface.
using CalibrateEquityVolEndpoint = quantra::ProductEndpoint<
    CalibrateEquityVolRequest,
    CalibrateEquityVolResponse,
    quantra::CalibrateEquityVolMapper,
    quantra::CalibrateEquityVolEvaluator>;

class CalibrateEquityVolData : public CallDataGeneric<
                                   CalibrateEquityVolRequest,
                                   CalibrateEquityVolEndpoint,
                                   CalibrateEquityVolResponse,
                                   CalibrateEquityVolResponseBuilder> {
public:
    CalibrateEquityVolData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestCalibrateEquityVol(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalibrateEquityVolData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalibrateEquityVol, CalibrateEquityVolData);

#endif // QUANTRASERVER_CALIBRATE_EQUITY_VOL_HANDLER_H
