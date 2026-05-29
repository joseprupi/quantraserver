#ifndef QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_HANDLER_H
#define QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_HANDLER_H

#include <exception>
#include <memory>
#include <string>

#include "bootstrap_inflation_curves_mapper.h"
#include "bootstrap_inflation_curves_pricer.h"
#include "bootstrap_inflation_curves_request_generated.h"
#include "bootstrap_inflation_curves_response_generated.h"
#include "call_data_base.h"
#include "eval_date_guard.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "quantra_request.h"

using quantra::BootstrapInflationCurvesRequest;
using quantra::BootstrapInflationCurvesResponse;
using quantra::BootstrapInflationCurvesResponseBuilder;

namespace quantra {

/// Endpoint binding for the BootstrapInflationCurves query. Mirrors the
/// generic ProductEndpoint glue but additionally catches registry-build
/// failures and embeds the message into each query's per-item error,
/// preserving the legacy response shape (top-level response with per-curve
/// Error entries rather than a transport-level INVALID_ARGUMENT).
class BootstrapInflationCurvesEndpoint
    : public QuantraRequest<BootstrapInflationCurvesRequest,
                            BootstrapInflationCurvesResponse> {
public:
    flatbuffers::Offset<BootstrapInflationCurvesResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const BootstrapInflationCurvesRequest* req) const override
    {
        EvalDateGuard guard;
        auto inputs = mapper_.toInputs(req);

        PricingRegistry reg;
        PricingContext ctx;
        std::string buildError;
        try {
            reg = PricingRegistryBuilder{}.build(req->pricing());
            ctx = makeContext(req->pricing(), reg);
        } catch (const std::exception& e) {
            buildError = e.what();
        }
        if (!buildError.empty()) {
            for (auto& q : inputs.queries) {
                if (q.prebuiltError.empty()) {
                    q.prebuiltError = buildError;
                }
            }
        }

        auto result = pricer_.price(inputs, reg, ctx);
        return mapper_.toResponse(*builder, result);
    }

private:
    BootstrapInflationCurvesMapper mapper_;
    BootstrapInflationCurvesPricer pricer_;
};

} // namespace quantra

/// Transitional alias: the C++ parity test instantiates this name directly.
using BootstrapInflationCurvesRequestHandler = quantra::BootstrapInflationCurvesEndpoint;

/**
 * BootstrapInflationCurvesData - Async handler for inflation curve bootstrapping.
 */
class BootstrapInflationCurvesData : public CallDataGeneric<
    BootstrapInflationCurvesRequest,
    quantra::BootstrapInflationCurvesEndpoint,
    BootstrapInflationCurvesResponse,
    BootstrapInflationCurvesResponseBuilder>
{
public:
    BootstrapInflationCurvesData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestBootstrapInflationCurves(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new BootstrapInflationCurvesData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(BootstrapInflationCurves, BootstrapInflationCurvesData);

#endif // QUANTRASERVER_BOOTSTRAP_INFLATION_CURVES_HANDLER_H
