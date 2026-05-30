#ifndef QUANTRA_BOOTSTRAP_INFLATION_CURVES_MAPPER_H
#define QUANTRA_BOOTSTRAP_INFLATION_CURVES_MAPPER_H

/**
 * BootstrapInflationCurvesMapper — the one place this product's flatbuffers
 * live. Decodes a BootstrapInflationCurvesRequest into plain
 * BootstrapInflationCurvesInputs (grid dates pre-resolved against the FB
 * inflation curve spec's reference_date) and serializes the result back
 * into a BootstrapInflationCurvesResponse.
 */

#include "flatbuffers/grpc.h"

#include "bootstrap_inflation_curves_pricer.h"
#include "bootstrap_inflation_curves_request_generated.h"
#include "bootstrap_inflation_curves_response_generated.h"

namespace quantra {

class BootstrapInflationCurvesMapper {
public:
    BootstrapInflationCurvesInputs toInputs(
        const quantra::BootstrapInflationCurvesRequest* req) const;

    flatbuffers::Offset<quantra::BootstrapInflationCurvesResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const BootstrapInflationCurvesResult& result) const;

    /// Registry-build-failure hook used by the generic ProductEndpoint glue.
    /// Folds the build-error message into each query's per-item error field so
    /// the pricer reports it as a per-curve Error entry (HTTP 200 list) rather
    /// than letting the failure surface as a transport-level error.
    void onRegistryBuildError(BootstrapInflationCurvesInputs& inputs,
                              const std::string& message) const;
};

} // namespace quantra

#endif // QUANTRA_BOOTSTRAP_INFLATION_CURVES_MAPPER_H
