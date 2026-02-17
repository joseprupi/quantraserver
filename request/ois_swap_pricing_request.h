#ifndef QUANTRASERVER_OIS_SWAP_PRICING_REQUEST_H
#define QUANTRASERVER_OIS_SWAP_PRICING_REQUEST_H

#include "flatbuffers/grpc.h"
#include "quantra_request.h"
#include "price_ois_swap_request_generated.h"
#include "ois_swap_response_generated.h"

class OisSwapPricingRequest : QuantraRequest<quantra::PriceOisSwapRequest,
                                             quantra::PriceOisSwapResponse> {
public:
    flatbuffers::Offset<quantra::PriceOisSwapResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceOisSwapRequest *request) const;
};

#endif // QUANTRASERVER_OIS_SWAP_PRICING_REQUEST_H
