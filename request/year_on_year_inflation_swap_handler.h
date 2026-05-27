#ifndef QUANTRASERVER_YEAR_ON_YEAR_INFLATION_SWAP_HANDLER_H
#define QUANTRASERVER_YEAR_ON_YEAR_INFLATION_SWAP_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "year_on_year_inflation_swap_mapper.h"
#include "year_on_year_inflation_swap_pricer.h"
#include "price_year_on_year_inflation_swap_request_generated.h"
#include "year_on_year_inflation_swap_response_generated.h"

using quantra::PriceYearOnYearInflationSwapRequest;
using quantra::PriceYearOnYearInflationSwapResponse;
using quantra::PriceYearOnYearInflationSwapResponseBuilder;

/// Wave-A product: year-on-year inflation swap (YYIIS). Mirrors the ZCIIS
/// cutover — the generic ProductEndpoint template owns the
/// Verify → mapper.toInputs → registry → context → pricer.price →
/// mapper.toResponse glue.
using YearOnYearInflationSwapEndpoint = quantra::ProductEndpoint<
    PriceYearOnYearInflationSwapRequest,
    PriceYearOnYearInflationSwapResponse,
    quantra::YearOnYearInflationSwapMapper,
    quantra::YearOnYearInflationSwapPricer>;

/// Backward-compatible alias for existing call sites (tests) that referenced
/// the legacy YearOnYearInflationSwapPricingRequest type directly.
using YearOnYearInflationSwapPricingRequest = YearOnYearInflationSwapEndpoint;

class PriceYearOnYearInflationSwapData : public CallDataGeneric<
    PriceYearOnYearInflationSwapRequest,
    YearOnYearInflationSwapEndpoint,
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
