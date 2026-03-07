#ifndef QUANTRASERVER_YEAR_ON_YEAR_INFLATION_SWAP_PRICING_REQUEST_H
#define QUANTRASERVER_YEAR_ON_YEAR_INFLATION_SWAP_PRICING_REQUEST_H

#include "flatbuffers/grpc.h"
#include "quantra_request.h"
#include "price_year_on_year_inflation_swap_request_generated.h"
#include "year_on_year_inflation_swap_response_generated.h"

class YearOnYearInflationSwapPricingRequest
    : QuantraRequest<quantra::PriceYearOnYearInflationSwapRequest,
                     quantra::PriceYearOnYearInflationSwapResponse> {
public:
    flatbuffers::Offset<quantra::PriceYearOnYearInflationSwapResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceYearOnYearInflationSwapRequest* request) const;
};

#endif // QUANTRASERVER_YEAR_ON_YEAR_INFLATION_SWAP_PRICING_REQUEST_H
