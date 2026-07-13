#ifndef QUANTRASERVER_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_HANDLER_H
#define QUANTRASERVER_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "year_on_year_inflation_cap_floor_mapper.h"
#include "year_on_year_inflation_cap_floor_evaluator.h"
#include "price_year_on_year_inflation_cap_floor_request_generated.h"
#include "year_on_year_inflation_cap_floor_response_generated.h"

using quantra::PriceYearOnYearInflationCapFloorRequest;
using quantra::PriceYearOnYearInflationCapFloorResponse;
using quantra::PriceYearOnYearInflationCapFloorResponseBuilder;

/// Generic endpoint binding for the year-on-year inflation cap/floor product.
/// The FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using YearOnYearInflationCapFloorEndpoint = quantra::ProductEndpoint<
    PriceYearOnYearInflationCapFloorRequest,
    PriceYearOnYearInflationCapFloorResponse,
    quantra::YearOnYearInflationCapFloorMapper,
    quantra::YearOnYearInflationCapFloorEvaluator>;

class PriceYearOnYearInflationCapFloorData : public CallDataGeneric<
    PriceYearOnYearInflationCapFloorRequest,
    YearOnYearInflationCapFloorEndpoint,
    PriceYearOnYearInflationCapFloorResponse,
    PriceYearOnYearInflationCapFloorResponseBuilder> {
public:
    PriceYearOnYearInflationCapFloorData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceYearOnYearInflationCapFloor(
            &ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceYearOnYearInflationCapFloorData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(PriceYearOnYearInflationCapFloor, PriceYearOnYearInflationCapFloorData);

#endif // QUANTRASERVER_YEAR_ON_YEAR_INFLATION_CAP_FLOOR_HANDLER_H
