#ifndef QUANTRASERVER_BASIS_SWAP_PRICING_REQUEST_H
#define QUANTRASERVER_BASIS_SWAP_PRICING_REQUEST_H

#include "flatbuffers/grpc.h"
#include "quantra_request.h"
#include "basis_swap_response_generated.h"
#include "price_basis_swap_request_generated.h"

class BasisSwapPricingRequest : QuantraRequest<quantra::PriceBasisSwapRequest,
                                               quantra::PriceBasisSwapResponse> {
public:
    flatbuffers::Offset<quantra::PriceBasisSwapResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::PriceBasisSwapRequest *request) const;
};

#endif // QUANTRASERVER_BASIS_SWAP_PRICING_REQUEST_H
