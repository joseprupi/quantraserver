#ifndef QUANTRASERVER_META_HANDLER_H
#define QUANTRASERVER_META_HANDLER_H

#include <memory>
#include <string>
#include <vector>

#include "flatbuffers/grpc.h"

#include "call_data_base.h"
#include "product_registry.h"
#include "product_catalog.h"
#include "meta_request_generated.h"
#include "meta_response_generated.h"

// Build/version metadata comes from compile definitions single-sourced in
// cmake/quantra_build_info.cmake (the SAME values the JSON gateway's /meta uses).
// The fallbacks only keep this header compilable if a define is ever missing.
#ifndef QUANTRA_API_VERSION
#define QUANTRA_API_VERSION "unknown"
#endif
#ifndef QUANTRA_BACKEND_VERSION
#define QUANTRA_BACKEND_VERSION "unknown"
#endif
#ifndef QUANTRA_GIT_SHA
#define QUANTRA_GIT_SHA "unknown"
#endif
#ifndef QUANTRA_BUILD_TIME_UTC
#define QUANTRA_BUILD_TIME_UTC "unknown"
#endif
#ifndef QUANTRA_QUANTLIB_VERSION
#define QUANTRA_QUANTLIB_VERSION "unknown"
#endif
#ifndef QUANTRA_GRPC_VERSION
#define QUANTRA_GRPC_VERSION "unknown"
#endif
#ifndef QUANTRA_FLATBUFFERS_VERSION
#define QUANTRA_FLATBUFFERS_VERSION "unknown"
#endif

using quantra::MetaRequest;
using quantra::MetaResponse;
using quantra::MetaResponseBuilder;

namespace quantra {

/// Assembles the MetaResponse for the Meta RPC. This lives in transport land
/// (it speaks FlatBuffers) and does NO pricing and touches NO market-data
/// registry: the reply is built purely from compile-time build metadata, the
/// shared product catalog, and the list of registered RPC handlers.
///
/// The type is duck-typed by CallDataGeneric (it only needs a default
/// constructor and a `request(builder, root)` member), mirroring how
/// ProductEndpoint plugs into the same base.
class MetaEndpoint {
public:
    flatbuffers::Offset<MetaResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const MetaRequest* /*req*/) const
    {
        auto& b = *builder;

        auto service = b.CreateString("quantra-grpc-engine");
        auto api_version = b.CreateString(QUANTRA_API_VERSION);
        auto backend_version = b.CreateString(QUANTRA_BACKEND_VERSION);
        auto git_sha = b.CreateString(QUANTRA_GIT_SHA);
        auto build_time = b.CreateString(QUANTRA_BUILD_TIME_UTC);

        // Product names: the SAME source the JSON gateway's /meta uses.
        std::vector<flatbuffers::Offset<flatbuffers::String>> product_offsets;
        for (const auto& entry : GetProductCatalog()) {
            product_offsets.push_back(b.CreateString(ProductTypeToString(entry.first)));
        }
        auto products = b.CreateVector(product_offsets);

        // RPC method names: the actual registered async handlers, which are
        // registered under their gRPC method names (see REGISTER_PRODUCT calls),
        // i.e. exactly the rpc_service methods in grpc/quantraserver.fbs,
        // including Meta itself.
        std::vector<flatbuffers::Offset<flatbuffers::String>> method_offsets;
        for (const auto& name : ProductRegistry::instance().getNames()) {
            method_offsets.push_back(b.CreateString(name));
        }
        auto rpc_methods = b.CreateVector(method_offsets);

        auto quantlib = b.CreateString(QUANTRA_QUANTLIB_VERSION);
        auto grpc_ver = b.CreateString(QUANTRA_GRPC_VERSION);
        auto flatbuffers_ver = b.CreateString(QUANTRA_FLATBUFFERS_VERSION);
        MetaDependenciesBuilder deps(b);
        deps.add_quantlib(quantlib);
        deps.add_grpc(grpc_ver);
        deps.add_flatbuffers(flatbuffers_ver);
        auto dependencies = deps.Finish();

        MetaResponseBuilder mrb(b);
        mrb.add_service(service);
        mrb.add_api_version(api_version);
        mrb.add_backend_version(backend_version);
        mrb.add_git_sha(git_sha);
        mrb.add_build_time_utc(build_time);
        mrb.add_products(products);
        mrb.add_rpc_methods(rpc_methods);
        mrb.add_dependencies(dependencies);
        return mrb.Finish();
    }
};

} // namespace quantra

/// Async CallData for the Meta RPC. Reuses the shared request lifecycle in
/// CallDataGeneric (deadline short-circuit, verify, single Finish), so it needs
/// only the four types and the two service hooks — same shape as every product
/// handler.
class MetaCallData : public CallDataGeneric<
    MetaRequest,
    quantra::MetaEndpoint,
    MetaResponse,
    MetaResponseBuilder>
{
public:
    MetaCallData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {
    }

    void RequestCall() override {
        service_->RequestMeta(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new MetaCallData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(Meta, MetaCallData);

#endif // QUANTRASERVER_META_HANDLER_H
