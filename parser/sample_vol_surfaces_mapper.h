#ifndef QUANTRA_SAMPLE_VOL_SURFACES_MAPPER_H
#define QUANTRA_SAMPLE_VOL_SURFACES_MAPPER_H

/**
 * SampleVolSurfacesMapper — the one place this product's flatbuffers live.
 * Decodes a SampleVolSurfacesRequest into plain SampleVolSurfacesInputs (each
 * query's surface id, family, grids, options and selectors lifted into plain
 * mirrors) and serializes a SampleVolSurfacesResult back into a
 * SampleVolSurfacesResponse, including the SwaptionVolDiagnostics sub-tables
 * via the shared diagnostics builders. A registry-build failure is folded into
 * each query's per-item error via onRegistryBuildError, so the response stays a
 * per-query VolSurfaceSample list (HTTP 200) rather than a whole-batch error.
 */

#include "flatbuffers/grpc.h"

#include "sample_vol_surfaces_evaluator.h"
#include "sample_vol_surfaces_request_generated.h"
#include "sample_vol_surfaces_response_generated.h"

namespace quantra {

class SampleVolSurfacesMapper {
public:
    SampleVolSurfacesInputs toInputs(const quantra::SampleVolSurfacesRequest* req) const;

    flatbuffers::Offset<quantra::SampleVolSurfacesResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const SampleVolSurfacesResult& result) const;

    /// Registry-build-failure hook used by the generic ProductEndpoint glue.
    /// Folds the build-error message into each query's per-item error field so
    /// the pricer reports it as a per-query VolSurfaceSample Error entry (HTTP
    /// 200 list) rather than letting the failure surface as a transport-level
    /// error.
    void onRegistryBuildError(SampleVolSurfacesInputs& inputs,
                              const std::string& message) const;
};

} // namespace quantra

#endif // QUANTRA_SAMPLE_VOL_SURFACES_MAPPER_H
