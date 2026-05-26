#ifndef QUANTRA_FIXED_RATE_BOND_MAPPER_H
#define QUANTRA_FIXED_RATE_BOND_MAPPER_H

/**
 * FixedRateBondMapper — the one place fixed-rate-bond flatbuffers live.
 * Hands a FixedRateBondInputs to the pricer and serializes the
 * FixedRateBondResult back into a PriceFixedRateBondResponse.
 */

#include "flatbuffers/grpc.h"

#include "fixed_rate_bond_pricer.h"
#include "price_fixed_rate_bond_request_generated.h"
#include "fixed_rate_bond_response_generated.h"

namespace quantra {

class FixedRateBondMapper {
public:
    FixedRateBondInputs toInputs(const quantra::PriceFixedRateBondRequest* req) const;

    flatbuffers::Offset<quantra::PriceFixedRateBondResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const FixedRateBondResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_FIXED_RATE_BOND_MAPPER_H
