#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <thread>

#include "data/fixed_rate_bond_request_fbs.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;

class QuantraClient
{
public:
    explicit QuantraClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(quantra::QuantraServer::NewStub(channel)) {}

    void PriceBond()
    {

        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        bond_request_fbs(builder);

        auto request_msg = builder->ReleaseMessage<quantra::PriceFixedRateBondRequest>();

        AsyncClientCall *call = new AsyncClientCall;

        call->response_reader =
            stub_->PrepareAsyncPriceFixedRateBond(&call->context, request_msg, &cq_);

        call->response_reader->StartCall();

        call->response_reader->Finish(&call->reply, &call->status, (void *)call);
    }

    void AsyncCompleteRpc(int n)
    {
        void *got_tag;
        bool ok = false;

        int req_count = 0;
        std::cout << "Start loop" << std::endl;
        while (cq_.Next(&got_tag, &ok))
        {
            AsyncClientCall *call = static_cast<AsyncClientCall *>(got_tag);

            GPR_ASSERT(ok);

            if (call->status.ok())
            {
                const quantra::PriceFixedRateBondResponse *response = call->reply.GetRoot();
                //auto npv = response->npv();
                //std::cout << "NPV: " << npv << std::endl;
            }
            else
                std::cout << "RPC failed" << call->status.error_code() << std::endl;

            delete call;

            req_count += 1;

            if (req_count == n)
                break;
        }
    }

private:
    struct AsyncClientCall
    {

        flatbuffers::grpc::Message<quantra::PriceFixedRateBondResponse> reply;
        grpc::ClientContext context;
        grpc::Status status;

        std::unique_ptr<grpc::ClientAsyncResponseReader<flatbuffers::grpc::Message<quantra::PriceFixedRateBondResponse>>> response_reader;
    };

    std::unique_ptr<quantra::QuantraServer::Stub> stub_;
    grpc::CompletionQueue cq_;
};

int main(int argc, char **argv)
{

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    QuantraClient quantra_client(grpc::CreateChannel(
        "localhost:50051", grpc::InsecureChannelCredentials()));

    int n = 1;

    if (argc > 1)
    {
        std::istringstream iss(argv[1]);
        iss >> n;
    }

    std::thread thread_ = std::thread(&QuantraClient::AsyncCompleteRpc, &quantra_client, n);

    for (int i = 0; i < n; i++)
    {
        quantra_client.PriceBond();
    }

    thread_.join();

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() << "[ms]" << std::endl;

    return 0;
}