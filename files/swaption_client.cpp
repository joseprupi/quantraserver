#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <thread>

#include "data/swaption_request_fbs.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;

class SwaptionClient
{
public:
    explicit SwaptionClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(quantra::QuantraServer::NewStub(channel)) {}

    void PriceSwaption()
    {
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = 
            std::make_shared<flatbuffers::grpc::MessageBuilder>();
        swaption_request_fbs(builder);

        auto request_msg = builder->ReleaseMessage<quantra::PriceSwaptionRequest>();

        AsyncClientCall *call = new AsyncClientCall;

        call->response_reader =
            stub_->PrepareAsyncPriceSwaption(&call->context, request_msg, &cq_);

        call->response_reader->StartCall();

        call->response_reader->Finish(&call->reply, &call->status, (void *)call);
    }

    void AsyncCompleteRpc(int n)
    {
        void *got_tag;
        bool ok = false;

        int req_count = 0;
        std::cout << "Waiting for responses..." << std::endl;
        
        while (cq_.Next(&got_tag, &ok))
        {
            AsyncClientCall *call = static_cast<AsyncClientCall *>(got_tag);

            GPR_ASSERT(ok);

            if (call->status.ok())
            {
                const quantra::PriceSwaptionResponse *response = call->reply.GetRoot();
                
                auto swaptions = response->swaptions();
                if (swaptions && swaptions->size() > 0)
                {
                    auto swaption = swaptions->Get(0);
                    
                    std::cout << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << "        SWAPTION PRICING RESULTS        " << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << std::fixed << std::setprecision(2);
                    std::cout << "NPV:              " << std::setw(15) << swaption->npv() << " EUR" << std::endl;
                    std::cout << std::fixed << std::setprecision(4);
                    std::cout << "Implied Vol:      " << std::setw(15) << swaption->implied_volatility() * 100 << " %" << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << std::endl;
                }
            }
            else
            {
                std::cout << "RPC failed: " << call->status.error_code() 
                          << " - " << call->status.error_message() << std::endl;
            }

            delete call;

            req_count += 1;

            if (req_count == n)
                break;
        }
    }

private:
    struct AsyncClientCall
    {
        flatbuffers::grpc::Message<quantra::PriceSwaptionResponse> reply;
        grpc::ClientContext context;
        grpc::Status status;

        std::unique_ptr<grpc::ClientAsyncResponseReader<
            flatbuffers::grpc::Message<quantra::PriceSwaptionResponse>>> response_reader;
    };

    std::unique_ptr<quantra::QuantraServer::Stub> stub_;
    grpc::CompletionQueue cq_;
};

int main(int argc, char **argv)
{
    std::cout << "========================================" << std::endl;
    std::cout << "       Swaption Pricing Client          " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Swaption Details:" << std::endl;
    std::cout << "  Type:          1Y x 5Y Payer Swaption" << std::endl;
    std::cout << "  Exercise:      European (2026-01-27)" << std::endl;
    std::cout << "  Settlement:    Physical" << std::endl;
    std::cout << "  Notional:      10,000,000 EUR" << std::endl;
    std::cout << "  Strike:        4.00%" << std::endl;
    std::cout << "  Volatility:    20% (Black)" << std::endl;
    std::cout << std::endl;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    SwaptionClient client(grpc::CreateChannel(
        "localhost:50051", grpc::InsecureChannelCredentials()));

    int n = 1;

    if (argc > 1)
    {
        std::istringstream iss(argv[1]);
        iss >> n;
    }

    std::thread thread_ = std::thread(&SwaptionClient::AsyncCompleteRpc, &client, n);

    for (int i = 0; i < n; i++)
    {
        client.PriceSwaption();
    }

    thread_.join();

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() 
              << "[ms]" << std::endl;

    return 0;
}
