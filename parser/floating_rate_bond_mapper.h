#ifndef QUANTRA_FLOATING_RATE_BOND_MAPPER_H
#define QUANTRA_FLOATING_RATE_BOND_MAPPER_H

/**
 * FloatingRateBondMapper — the one place floating-rate-bond flatbuffers live.
 * Decodes a PriceFloatingRateBondRequest into plain FloatingRateBondInputs and
 * serializes the FloatingRateBondResult back into a PriceFloatingRateBondResponse.
 */

#include "flatbuffers/grpc.h"

#include "floating_rate_bond_evaluator.h"
#include "floating_rate_bond_response_generated.h"
#include "price_floating_rate_bond_request_generated.h"

namespace quantra {

class FloatingRateBondMapper {
public:
    FloatingRateBondInputs toInputs(const quantra::PriceFloatingRateBondRequest* req) const;

    flatbuffers::Offset<quantra::PriceFloatingRateBondResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const FloatingRateBondResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_FLOATING_RATE_BOND_MAPPER_H
