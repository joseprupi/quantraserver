#ifndef QUANTRA_CALLABLE_FIXED_RATE_BOND_MAPPER_H
#define QUANTRA_CALLABLE_FIXED_RATE_BOND_MAPPER_H

/**
 * CallableFixedRateBondMapper — the one place callable-fixed-rate-bond
 * flatbuffers live. Decodes a PriceCallableFixedRateBondRequest into plain
 * CallableFixedRateBondInputs and serializes a CallableFixedRateBondResult back
 * into a PriceCallableFixedRateBondResponse.
 */

#include "callable_fixed_rate_bond_evaluator.h"

#include "callable_fixed_rate_bond_response_generated.h"
#include "price_callable_fixed_rate_bond_request_generated.h"

#include "flatbuffers/grpc.h"

namespace quantra {

class CallableFixedRateBondMapper {
public:
    CallableFixedRateBondInputs toInputs(
        const quantra::PriceCallableFixedRateBondRequest* req) const;

    flatbuffers::Offset<quantra::PriceCallableFixedRateBondResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CallableFixedRateBondResult& result) const;

    /// The response of a `price_only` request: one NPV per trade
    /// (price_only_response.h).
    flatbuffers::Offset<quantra::PriceCallableFixedRateBondResponse> toPriceOnlyResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CallableFixedRateBondResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CALLABLE_FIXED_RATE_BOND_MAPPER_H
