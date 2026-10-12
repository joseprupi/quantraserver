#ifndef QUANTRASERVER_FX_FORWARD_HANDLER_H
#define QUANTRASERVER_FX_FORWARD_HANDLER_H

#include "call_data_base.h"
#include "fx_forward_evaluator.h"
#include "fx_forward_mapper.h"
#include "product_endpoint.h"
#include "product_registry.h"

#include "fx_forward_response_generated.h"
#include "price_fx_forward_request_generated.h"

using quantra::PriceFxForwardRequest;
using quantra::PriceFxForwardResponse;
using quantra::PriceFxForwardResponseBuilder;

/// Generic endpoint binding for the FX forward product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using FxForwardEndpoint = quantra::ProductEndpoint<
    PriceFxForwardRequest,
    PriceFxForwardResponse,
    quantra::FxForwardMapper,
    quantra::FxForwardEvaluator>;

/**
 * PriceFxForwardData - Async handler for FX forward pricing.
 */
class PriceFxForwardData : public CallDataGeneric<
                               PriceFxForwardRequest,
                               FxForwardEndpoint,
                               PriceFxForwardResponse,
                               PriceFxForwardResponseBuilder> {
public:
    PriceFxForwardData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceFxForward(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceFxForwardData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceFxForward, PriceFxForwardData);

#endif // QUANTRASERVER_FX_FORWARD_HANDLER_H
