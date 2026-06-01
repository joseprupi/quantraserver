#ifndef QUANTRA_SWAP_LEG_FLOW_BUILDER_H
#define QUANTRA_SWAP_LEG_FLOW_BUILDER_H

#include <memory>

#include <flatbuffers/grpc.h>
#include <ql/cashflow.hpp>
#include <ql/time/date.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "basis_swap_response_generated.h"

namespace quantra {

flatbuffers::Offset<SwapLegFlow> buildSwapLegFlow(
    const std::shared_ptr<QuantLib::CashFlow>& cf,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    flatbuffers::grpc::MessageBuilder& builder,
    QuantLib::Date asOf);

} // namespace quantra

#endif // QUANTRA_SWAP_LEG_FLOW_BUILDER_H
