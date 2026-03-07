#ifndef QUANTRASERVER_YEAR_ON_YEAR_INFLATION_SWAP_HANDLER_H
#define QUANTRASERVER_YEAR_ON_YEAR_INFLATION_SWAP_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "year_on_year_inflation_swap_pricing_request.h"
#include "price_year_on_year_inflation_swap_request_generated.h"
#include "year_on_year_inflation_swap_response_generated.h"

using quantra::PriceYearOnYearInflationSwapRequest;
using quantra::PriceYearOnYearInflationSwapResponse;
using quantra::PriceYearOnYearInflationSwapResponseBuilder;

class PriceYearOnYearInflationSwapData : public CallDataGeneric<
    PriceYearOnYearInflationSwapRequest,
    YearOnYearInflationSwapPricingRequest,
    PriceYearOnYearInflationSwapResponse,
    PriceYearOnYearInflationSwapResponseBuilder> {
public:
    PriceYearOnYearInflationSwapData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceYearOnYearInflationSwap(
            &ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceYearOnYearInflationSwapData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(PriceYearOnYearInflationSwap, PriceYearOnYearInflationSwapData);

#endif // QUANTRASERVER_YEAR_ON_YEAR_INFLATION_SWAP_HANDLER_H
