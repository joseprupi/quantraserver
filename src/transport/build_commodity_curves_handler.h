#ifndef QUANTRASERVER_BUILD_COMMODITY_CURVES_HANDLER_H
#define QUANTRASERVER_BUILD_COMMODITY_CURVES_HANDLER_H

#include "call_data_base.h"
#include "commodity_curve_build_evaluator.h"
#include "commodity_curve_build_mapper.h"
#include "product_endpoint.h"
#include "product_registry.h"

#include "build_commodity_curves_request_generated.h"
#include "build_commodity_curves_response_generated.h"

using quantra::BuildCommodityCurvesRequest;
using quantra::BuildCommodityCurvesResponse;
using quantra::BuildCommodityCurvesResponseBuilder;

/// Generic endpoint binding for the commodity curve build. The
/// FlatBuffers→domain→library→FlatBuffers glue lives in ProductEndpoint; the
/// mapper opts out of the pricing registry (no curve is needed).
using BuildCommodityCurvesEndpoint = quantra::ProductEndpoint<
    BuildCommodityCurvesRequest,
    BuildCommodityCurvesResponse,
    quantra::CommodityCurveBuildMapper,
    quantra::CommodityCurveBuildEvaluator>;

/**
 * BuildCommodityCurvesData - Async handler for the commodity curve build.
 */
class BuildCommodityCurvesData : public CallDataGeneric<
                                     BuildCommodityCurvesRequest,
                                     BuildCommodityCurvesEndpoint,
                                     BuildCommodityCurvesResponse,
                                     BuildCommodityCurvesResponseBuilder> {
public:
    BuildCommodityCurvesData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestBuildCommodityCurves(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new BuildCommodityCurvesData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(BuildCommodityCurves, BuildCommodityCurvesData);

#endif // QUANTRASERVER_BUILD_COMMODITY_CURVES_HANDLER_H
