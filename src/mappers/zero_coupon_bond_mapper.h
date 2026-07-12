#ifndef QUANTRA_ZERO_COUPON_BOND_MAPPER_H
#define QUANTRA_ZERO_COUPON_BOND_MAPPER_H

/**
 * ZeroCouponBondMapper — the one place zero-coupon-bond flatbuffers live.
 * Decodes a PriceZeroCouponBondRequest into plain ZeroCouponBondInputs and
 * serializes the ZeroCouponBondResult back into a PriceZeroCouponBondResponse.
 */

#include "flatbuffers/grpc.h"

#include "zero_coupon_bond_evaluator.h"
#include "price_zero_coupon_bond_request_generated.h"
#include "zero_coupon_bond_response_generated.h"

namespace quantra {

class ZeroCouponBondMapper {
public:
    ZeroCouponBondInputs toInputs(const quantra::PriceZeroCouponBondRequest* req) const;

    flatbuffers::Offset<quantra::PriceZeroCouponBondResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const ZeroCouponBondResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_ZERO_COUPON_BOND_MAPPER_H
