#ifndef QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_REQUEST_H
#define QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_REQUEST_H

#include <ql/quantlib.hpp>

#include "flatbuffers/grpc.h"
#include "bootstrap_inflation_curves_request_generated.h"
#include "bootstrap_inflation_curves_response_generated.h"
#include "pricing_registry.h"

class BootstrapInflationCurvesRequestHandler {
public:
    flatbuffers::Offset<quantra::BootstrapInflationCurvesResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::BootstrapInflationCurvesRequest* request) const;
};

#endif // QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_REQUEST_H
