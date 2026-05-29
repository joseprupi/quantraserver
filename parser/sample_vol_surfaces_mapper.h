#ifndef QUANTRA_SAMPLE_VOL_SURFACES_MAPPER_H
#define QUANTRA_SAMPLE_VOL_SURFACES_MAPPER_H

/**
 * SampleVolSurfacesMapper — the one place this product's flatbuffers live.
 * Decodes a SampleVolSurfacesRequest into plain SampleVolSurfacesInputs (each
 * query's surface id, family, grids, options and selectors lifted into plain
 * mirrors) and serializes a SampleVolSurfacesResult back into a
 * SampleVolSurfacesResponse, including the SwaptionVolDiagnostics sub-tables
 * via the shared diagnostics builders. toErrorResponse reproduces the legacy
 * top-level error shape (one per-query VolSurfaceSample carrying the message).
 */

#include "flatbuffers/grpc.h"

#include "sample_vol_surfaces_pricer.h"
#include "sample_vol_surfaces_request_generated.h"
#include "sample_vol_surfaces_response_generated.h"

namespace quantra {

class SampleVolSurfacesMapper {
public:
    SampleVolSurfacesInputs toInputs(const quantra::SampleVolSurfacesRequest* req) const;

    flatbuffers::Offset<quantra::SampleVolSurfacesResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const SampleVolSurfacesResult& result) const;

    flatbuffers::Offset<quantra::SampleVolSurfacesResponse> toErrorResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const quantra::SampleVolSurfacesRequest* req,
        const std::string& message) const;
};

} // namespace quantra

#endif // QUANTRA_SAMPLE_VOL_SURFACES_MAPPER_H
