#ifndef QUANTRASERVER_EQUITY_OPTION_PRICING_REQUEST_H
#define QUANTRASERVER_EQUITY_OPTION_PRICING_REQUEST_H

#include "flatbuffers/grpc.h"

#include "quantra_request.h"

#include "price_equity_option_request_generated.h"
#include "equity_option_response_generated.h"

class EquityOptionPricingRequest
    : QuantraRequest<quantra::PriceEquityOptionRequest, quantra::PriceEquityOptionResponse> {
public:
    flatbuffers::Offset<quantra::PriceEquityOptionResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceEquityOptionRequest* request) const;
};

#endif // QUANTRASERVER_EQUITY_OPTION_PRICING_REQUEST_H
