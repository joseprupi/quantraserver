#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <thread>

#include "data/fra_request_fbs.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;

class FRAClient
{
public:
    explicit FRAClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(quantra::QuantraServer::NewStub(channel)) {}

    void PriceFRA()
    {
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = 
            std::make_shared<flatbuffers::grpc::MessageBuilder>();
        fra_request_fbs(builder);

        auto request_msg = builder->ReleaseMessage<quantra::PriceFRARequest>();

        AsyncClientCall *call = new AsyncClientCall;

        call->response_reader =
            stub_->PrepareAsyncPriceFRA(&call->context, request_msg, &cq_);

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
                const quantra::PriceFRAResponse *response = call->reply.GetRoot();
                
                auto fras = response->fras();
                if (fras && fras->size() > 0)
                {
                    auto fra = fras->Get(0);
                    
                    std::cout << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << "            FRA RESULTS                 " << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << std::fixed << std::setprecision(2);
                    std::cout << "NPV:              " << std::setw(15) << fra->npv() << " EUR" << std::endl;
                    std::cout << std::fixed << std::setprecision(6);
                    std::cout << "Forward Rate:     " << std::setw(15) << fra->forward_rate() * 100 << " %" << std::endl;
                    std::cout << std::fixed << std::setprecision(2);
                    std::cout << "Spot Value:       " << std::setw(15) << fra->spot_value() << " EUR" << std::endl;
                    std::cout << "Settlement Date:  " << std::setw(15) << fra->settlement_date()->str() << std::endl;
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
        flatbuffers::grpc::Message<quantra::PriceFRAResponse> reply;
        grpc::ClientContext context;
        grpc::Status status;

        std::unique_ptr<grpc::ClientAsyncResponseReader<
            flatbuffers::grpc::Message<quantra::PriceFRAResponse>>> response_reader;
    };

    std::unique_ptr<quantra::QuantraServer::Stub> stub_;
    grpc::CompletionQueue cq_;
};

int main(int argc, char **argv)
{
    std::cout << "========================================" << std::endl;
    std::cout << "       FRA Pricing Client               " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "FRA Details:" << std::endl;
    std::cout << "  Type:        3x6 FRA (Long)" << std::endl;
    std::cout << "  Notional:    10,000,000 EUR" << std::endl;
    std::cout << "  Strike:      3.50%" << std::endl;
    std::cout << "  Start:       2025-04-29 (3M)" << std::endl;
    std::cout << "  Maturity:    2025-07-29 (6M)" << std::endl;
    std::cout << "  Index:       3M Euribor" << std::endl;
    std::cout << std::endl;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    FRAClient client(grpc::CreateChannel(
        "localhost:50051", grpc::InsecureChannelCredentials()));

    int n = 1;

    if (argc > 1)
    {
        std::istringstream iss(argv[1]);
        iss >> n;
    }

    std::thread thread_ = std::thread(&FRAClient::AsyncCompleteRpc, &client, n);

    for (int i = 0; i < n; i++)
    {
        client.PriceFRA();
    }

    thread_.join();

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() 
              << "[ms]" << std::endl;

    return 0;
}
