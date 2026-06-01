#ifndef QUANTRASERVER_SWAPTION_HANDLER_H
#define QUANTRASERVER_SWAPTION_HANDLER_H

#include "call_data_base.h"
#include "price_swaption_request_generated.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "swaption_mapper.h"
#include "swaption_evaluator.h"
#include "swaption_response_generated.h"

using quantra::PriceSwaptionRequest;
using quantra::PriceSwaptionResponse;
using quantra::PriceSwaptionResponseBuilder;

/// Wave-A product: swaption. Mirrors the CDS/YYIIS/ZCIIS cutovers — the
/// generic ProductEndpoint template owns the Verify → mapper.toInputs →
/// registry → context → pricer.price → mapper.toResponse glue.
using SwaptionEndpoint = quantra::ProductEndpoint<
    PriceSwaptionRequest,
    PriceSwaptionResponse,
    quantra::SwaptionMapper,
    quantra::SwaptionEvaluator>;

/// Backward-compatible alias for existing call sites (tests) that referenced
/// the legacy SwaptionPricingRequest type directly.
using SwaptionPricingRequest = SwaptionEndpoint;

class PriceSwaptionData : public CallDataGeneric<
    PriceSwaptionRequest,
    SwaptionEndpoint,
    PriceSwaptionResponse,
    PriceSwaptionResponseBuilder>
{
public:
    PriceSwaptionData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceSwaption(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceSwaptionData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceSwaption, PriceSwaptionData);

#endif // QUANTRASERVER_SWAPTION_HANDLER_H
