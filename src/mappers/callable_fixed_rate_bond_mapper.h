#ifndef QUANTRA_CALLABLE_FIXED_RATE_BOND_MAPPER_H
#define QUANTRA_CALLABLE_FIXED_RATE_BOND_MAPPER_H

/**
 * CallableFixedRateBondMapper — the one place callable-fixed-rate-bond
 * flatbuffers live. Decodes a PriceCallableFixedRateBondRequest into plain
 * CallableFixedRateBondInputs and serializes a CallableFixedRateBondResult back
 * into a PriceCallableFixedRateBondResponse.
 */

#include "flatbuffers/grpc.h"

#include "callable_fixed_rate_bond_evaluator.h"
#include "price_callable_fixed_rate_bond_request_generated.h"
#include "callable_fixed_rate_bond_response_generated.h"

namespace quantra {

class CallableFixedRateBondMapper {
public:
    CallableFixedRateBondInputs toInputs(
        const quantra::PriceCallableFixedRateBondRequest* req) const;

    flatbuffers::Offset<quantra::PriceCallableFixedRateBondResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CallableFixedRateBondResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CALLABLE_FIXED_RATE_BOND_MAPPER_H
