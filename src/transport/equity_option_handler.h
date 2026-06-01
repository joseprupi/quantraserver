#ifndef QUANTRASERVER_EQUITY_OPTION_HANDLER_H
#define QUANTRASERVER_EQUITY_OPTION_HANDLER_H

#include "call_data_base.h"
#include "equity_option_mapper.h"
#include "equity_option_evaluator.h"
#include "equity_option_response_generated.h"
#include "price_equity_option_request_generated.h"
#include "product_endpoint.h"
#include "product_registry.h"

using quantra::PriceEquityOptionRequest;
using quantra::PriceEquityOptionResponse;
using quantra::PriceEquityOptionResponseBuilder;

/// Generic endpoint binding for the EquityOption product.
using EquityOptionEndpoint = quantra::ProductEndpoint<
    PriceEquityOptionRequest,
    PriceEquityOptionResponse,
    quantra::EquityOptionMapper,
    quantra::EquityOptionEvaluator>;

/// Transitional alias for any caller that still names the legacy type.
using EquityOptionPricingRequest = EquityOptionEndpoint;

/**
 * PriceEquityOptionData - Async handler for EquityOption pricing.
 */
class PriceEquityOptionData : public CallDataGeneric<
    PriceEquityOptionRequest,
    EquityOptionEndpoint,
    PriceEquityOptionResponse,
    PriceEquityOptionResponseBuilder> {
public:
    PriceEquityOptionData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceEquityOption(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceEquityOptionData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(PriceEquityOption, PriceEquityOptionData);

#endif // QUANTRASERVER_EQUITY_OPTION_HANDLER_H
