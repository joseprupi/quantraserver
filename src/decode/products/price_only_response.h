#ifndef QUANTRA_PRICE_ONLY_RESPONSE_H
#define QUANTRA_PRICE_ONLY_RESPONSE_H

/**
 * The response of a pricing request with `price_only` set, shared by every
 * product mapper: the per-trade list present (it is required) but empty, and
 * `npvs` holding one NPV per trade in request order. A mapper's
 * `toPriceOnlyResponse` names its rows, how a row gives its NPV and the
 * response builder's list adder:
 *
 *   return PriceOnlyResponse(builder, result.swaps,
 *                            [](const auto& swap) { return swap.npv; },
 *                            &quantra::PriceVanillaSwapResponseBuilder::add_swaps);
 */

#include "flatbuffers/grpc.h"

#include <vector>

namespace quantra {

template <class RespBuilder, class Row, class Rows, class NpvOf>
flatbuffers::Offset<typename RespBuilder::Table> PriceOnlyResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const Rows& rows,
    NpvOf npvOf,
    void (RespBuilder::*addList)(
        flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<Row>>>)) {
    std::vector<double> npvs;
    npvs.reserve(rows.size());
    for (const auto& row : rows) {
        npvs.push_back(npvOf(row));
    }
    auto npvsVec = builder.CreateVector(npvs);
    auto empty = builder.CreateVector(std::vector<flatbuffers::Offset<Row>>{});
    RespBuilder rb(builder);
    (rb.*addList)(empty);
    rb.add_npvs(npvsVec);
    return rb.Finish();
}

} // namespace quantra

#endif // QUANTRA_PRICE_ONLY_RESPONSE_H
