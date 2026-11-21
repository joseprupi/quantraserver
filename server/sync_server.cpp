#include "call_data_base.h"
#include "product_registry.h"

#include "quantraserver.grpc.fb.h"

// Include all product handlers - they auto-register via REGISTER_PRODUCT macro
// ADD NEW PRODUCTS HERE (only change needed when adding products)
#include "basis_swap_handler.h"
#include "bootstrap_curves_handler.h"
#include "bootstrap_inflation_curves_handler.h"
#include "build_commodity_curves_handler.h"
#include "calendar_advance_handler.h"
#include "calendar_business_days_handler.h"
#include "calendar_holidays_handler.h"
#include "calibrate_correlation_handler.h"
#include "calibrate_implied_forwards_handler.h"
#include "calibrate_swaption_model_handler.h"
#include "calibrate_swaption_vol_handler.h"
#include "calibrate_vol_surface_handler.h"
#include "callable_fixed_rate_bond_handler.h"
#include "cap_floor_handler.h"
#include "cds_handler.h"
#include "commodity_swap_handler.h"
#include "cross_currency_swap_handler.h"
#include "equity_option_handler.h"
#include "fixed_rate_bond_handler.h"
#include "floating_rate_bond_handler.h"
#include "fra_handler.h"
#include "fx_forward_handler.h"
#include "ois_swap_handler.h"
#include "sample_vol_surfaces_handler.h"
#include "structured_note_handler.h"
#include "swaption_handler.h"
#include "validate_vol_surface_handler.h"
#include "vanilla_swap_handler.h"
#include "year_on_year_inflation_cap_floor_handler.h"
#include "year_on_year_inflation_swap_handler.h"
#include "zero_coupon_bond_handler.h"
#include "zero_coupon_inflation_swap_handler.h"
#include "zero_coupon_swap_handler.h"

// Service-metadata RPC (gRPC-only). Not a pricing product, but registers via the
// same mechanism so the completion-queue loop serves it uniformly.
#include "meta_handler.h"

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using quantra::QuantraServer;

namespace {

constexpr int kMaxGrpcMessageBytes = 8 * 1024 * 1024;
volatile std::sig_atomic_t g_shutdown_requested = 0;

void HandleShutdownSignal(int /*signum*/) {
    g_shutdown_requested = 1;
}

std::string GetBindHost() {
    const char* env_host = std::getenv("QUANTRA_SERVER_BIND_HOST");
    if (env_host == nullptr || env_host[0] == '\0') {
        return "127.0.0.1";
    }
    return env_host;
}

} // namespace

/**
 * ServerImpl - Async gRPC server.
 *
 * This file should NEVER need modification when adding new products.
 * Just create a new handler file with REGISTER_PRODUCT and #include it above.
 */
class ServerImpl final {
public:
    ~ServerImpl() { Shutdown(); }

    void Run(std::string port) {
        std::signal(SIGINT, HandleShutdownSignal);
        std::signal(SIGTERM, HandleShutdownSignal);

        std::string server_address(GetBindHost() + ":" + port);

        // Expose the standard grpc.health.v1.Health service (Check/Watch) with
        // zero extra codegen. Must be enabled before the server is built.
        grpc::EnableDefaultHealthCheckService(true);

        grpc::ServerBuilder builder;
        // Intentionally cap request/response sizes to harden against oversized payloads.
        builder.SetMaxReceiveMessageSize(kMaxGrpcMessageBytes);
        builder.SetMaxSendMessageSize(kMaxGrpcMessageBytes);
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service_);
        cq_ = builder.AddCompletionQueue();
        // The Meta RPC gets its own queue, drained by its own thread, so it
        // answers while this worker's single pricing thread is busy.
        meta_cq_ = builder.AddCompletionQueue();
        server_ = builder.BuildAndStart();

        // The default health service reports SERVING once the server is up; set
        // the overall ("") status explicitly so probes get a definite answer.
        if (auto* health = server_->GetHealthCheckService()) {
            health->SetServingStatus(true);
        }

        std::cout << "Server listening on " << server_address << std::endl;

        // Log registered products
        const auto& products = quantra::ProductRegistry::instance().getNames();
        std::cout << "Registered products (" << products.size() << "):" << std::endl;
        for (const auto& name : products) {
            std::cout << "  - " << name << std::endl;
        }

        HandleRpcs();
        Shutdown();
    }

private:
    void Shutdown() {
        if (shutdown_started_) {
            return;
        }

        shutdown_started_ = true;
        if (server_) {
            server_->Shutdown();
        }
        if (cq_) {
            cq_->Shutdown();
        }
        if (meta_cq_) {
            meta_cq_->Shutdown();
        }
        if (meta_thread_.joinable() && meta_thread_.get_id() != std::this_thread::get_id()) {
            meta_thread_.join();
        }
    }

    // Drains the Meta queue. Meta builds its reply from build metadata and the
    // product catalog only (no pricing, no market data, no shared mutable
    // state), so running it beside the pricing thread is safe.
    void HandleMetaRpcs() {
        void* tag = nullptr;
        bool ok = false;
        while (meta_cq_->Next(&tag, &ok)) {
            static_cast<CallData*>(tag)->Proceed(ok);
        }
    }

    void HandleRpcs() {
        // Initialize all registered products; Meta on its own queue and thread
        auto& registry = quantra::ProductRegistry::instance();
        registry.initializeAll(&service_, cq_.get(), {kMetaRpcName});
        registry.initialize(kMetaRpcName, &service_, meta_cq_.get());
        meta_thread_ = std::thread([this] { HandleMetaRpcs(); });

        // Main event loop
        while (true) {
            if (g_shutdown_requested != 0) {
                Shutdown();
            }

            void* tag = nullptr;
            bool ok = false;
            const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(200);
            const auto next_status = cq_->AsyncNext(&tag, &ok, deadline);

            if (next_status == grpc::CompletionQueue::TIMEOUT) {
                continue;
            }
            if (next_status == grpc::CompletionQueue::SHUTDOWN) {
                break;
            }

            static_cast<CallData*>(tag)->Proceed(ok);
        }
    }

    static constexpr const char* kMetaRpcName = "Meta";

    std::unique_ptr<grpc::ServerCompletionQueue> cq_;
    std::unique_ptr<grpc::ServerCompletionQueue> meta_cq_;
    std::thread meta_thread_;
    QuantraServer::AsyncService service_;
    std::unique_ptr<grpc::Server> server_;
    bool shutdown_started_ = false;
};

int main(int argc, char** argv) {
    std::string port = (argc > 1) ? argv[1] : "50051";

    ServerImpl server;
    server.Run(port);

    return 0;
}