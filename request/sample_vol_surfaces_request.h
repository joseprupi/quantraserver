#ifndef QUANTRA_SAMPLE_VOL_SURFACES_REQUEST_H
#define QUANTRA_SAMPLE_VOL_SURFACES_REQUEST_H

#include <ql/quantlib.hpp>

#include "flatbuffers/grpc.h"

#include "sample_vol_surfaces_request_generated.h"
#include "sample_vol_surfaces_response_generated.h"
#include "pricing_registry.h"

class SampleVolSurfacesRequestHandler {
public:
    flatbuffers::Offset<quantra::SampleVolSurfacesResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::SampleVolSurfacesRequest* request) const;
};

#endif // QUANTRA_SAMPLE_VOL_SURFACES_REQUEST_H
