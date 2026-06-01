#ifndef QUANTRA_BOND_FLOW_BUILDER_H
#define QUANTRA_BOND_FLOW_BUILDER_H

#include <memory>
#include <vector>

#include <flatbuffers/grpc.h>
#include <ql/cashflow.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "fixed_rate_bond_response_generated.h"
#include "floating_rate_bond_response_generated.h"

namespace quantra {

std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> buildFixedBondFlows(
    const QuantLib::Leg& cashflows,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const QuantLib::Date& asOf);

std::vector<flatbuffers::Offset<quantra::FlowsWrapper>> buildFloatingBondFlows(
    const QuantLib::Leg& cashflows,
    const std::shared_ptr<QuantLib::YieldTermStructure>& discountCurve,
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const QuantLib::Date& asOf);

} // namespace quantra

#endif // QUANTRA_BOND_FLOW_BUILDER_H
