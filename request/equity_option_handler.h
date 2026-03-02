#ifndef QUANTRASERVER_EQUITY_OPTION_HANDLER_H
#define QUANTRASERVER_EQUITY_OPTION_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "equity_option_pricing_request.h"
#include "price_equity_option_request_generated.h"
#include "equity_option_response_generated.h"

using quantra::PriceEquityOptionRequest;
using quantra::PriceEquityOptionResponse;
using quantra::PriceEquityOptionResponseBuilder;

class PriceEquityOptionData : public CallDataGeneric<
    PriceEquityOptionRequest,
    EquityOptionPricingRequest,
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
