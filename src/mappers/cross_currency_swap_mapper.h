#ifndef QUANTRA_CROSS_CURRENCY_SWAP_MAPPER_H
#define QUANTRA_CROSS_CURRENCY_SWAP_MAPPER_H

/**
 * CrossCurrencySwapMapper — the one place cross-currency swap flatbuffers
 * live. Decodes a PriceCrossCurrencySwapRequest (two `SwapLeg`s) into plain
 * CrossCurrencySwapInputs (validating every field that can be checked without
 * market data) and serializes the CrossCurrencySwapResult back into a
 * PriceCrossCurrencySwapResponse.
 */

#include "cross_currency_swap_evaluator.h"

#include "cross_currency_swap_response_generated.h"
#include "price_cross_currency_swap_request_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class CrossCurrencySwapMapper {
public:
    CrossCurrencySwapInputs toInputs(const quantra::PriceCrossCurrencySwapRequest* req) const;

    flatbuffers::Offset<quantra::PriceCrossCurrencySwapResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder, const CrossCurrencySwapResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CROSS_CURRENCY_SWAP_MAPPER_H
