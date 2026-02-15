#include "quantraserver.grpc.fb.h"
#include "product_registry.h"
#include "call_data_base.h"

// Include all product handlers - they auto-register via REGISTER_PRODUCT macro
// ADD NEW PRODUCTS HERE (only change needed when adding products)
#include "fixed_rate_bond_handler.h"
#include "floating_rate_bond_handler.h"
#include "vanilla_swap_handler.h"
#include "fra_handler.h"
#include "cap_floor_handler.h"
#include "swaption_handler.h"
#include "cds_handler.h"
#include "bootstrap_curves_handler.h"
#include "sample_vol_surfaces_handler.h"

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <string>

using quantra::QuantraServer;

/**
 * ServerImpl - Async gRPC server.
 *
 * This file should NEVER need modification when adding new products.
 * Just create a new handler file with REGISTER_PRODUCT and #include it above.
 */
class ServerImpl final
{
public:
    ~ServerImpl()
    {
        server_->Shutdown();
        cq_->Shutdown();
    }

    void Run(std::string port)
    {
        std::string server_address("127.0.0.1:" + port);

        grpc::ServerBuilder builder;
        builder.SetMaxMessageSize(INT_MAX);
        builder.SetMaxReceiveMessageSize(INT_MAX);
        builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
        builder.RegisterService(&service_);
        cq_ = builder.AddCompletionQueue();
        server_ = builder.BuildAndStart();

        std::cout << "Server listening on " << server_address << std::endl;

        // Log registered products
        const auto &products = quantra::ProductRegistry::instance().getNames();
        std::cout << "Registered products (" << products.size() << "):" << std::endl;
        for (const auto &name : products)
        {
            std::cout << "  - " << name << std::endl;
        }

        HandleRpcs();
    }

private:
    void HandleRpcs()
    {
        void *tag;
        bool ok;

        // Initialize all registered products
        quantra::ProductRegistry::instance().initializeAll(&service_, cq_.get());

        // Main event loop
        while (true)
        {
            GPR_ASSERT(cq_->Next(&tag, &ok));
            GPR_ASSERT(ok);
            static_cast<CallData *>(tag)->Proceed();
        }
    }

    std::unique_ptr<grpc::ServerCompletionQueue> cq_;
    QuantraServer::AsyncService service_;
    std::unique_ptr<grpc::Server> server_;
};

int main(int argc, char **argv)
{
    std::string port = (argc > 1) ? argv[1] : "50051";

    ServerImpl server;
    server.Run(port);

    return 0;
}