#ifndef QUANTRA_VANILLA_SWAP_FLOW_BUILDER_H
#define QUANTRA_VANILLA_SWAP_FLOW_BUILDER_H

#include <memory>
#include <vector>

#include <flatbuffers/grpc.h>
#include <ql/cashflow.hpp>
#include <ql/time/date.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "vanilla_swap_response_generated.h"

namespace quantra {

struct VanillaSwapFlowsBuildResult {
    std::vector<flatbuffers::Offset<SwapLegFlow>> fixedFlows;
    std::vector<flatbuffers::Offset<SwapLegFlow>> floatingFlows;
};

VanillaSwapFlowsBuildResult buildVanillaSwapFlows(
    const QuantLib::Leg& fixedLeg,
    const QuantLib::Leg& floatingLeg,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const QuantLib::Date& asOf);

} // namespace quantra

#endif // QUANTRA_VANILLA_SWAP_FLOW_BUILDER_H
