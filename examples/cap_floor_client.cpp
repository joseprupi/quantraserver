#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <thread>

#include "data/cap_floor_request_fbs.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;

class CapFloorClient
{
public:
    explicit CapFloorClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(quantra::QuantraServer::NewStub(channel)) {}

    void PriceCapFloor()
    {
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = 
            std::make_shared<flatbuffers::grpc::MessageBuilder>();
        cap_floor_request_fbs(builder);

        auto request_msg = builder->ReleaseMessage<quantra::PriceCapFloorRequest>();

        AsyncClientCall *call = new AsyncClientCall;

        call->response_reader =
            stub_->PrepareAsyncPriceCapFloor(&call->context, request_msg, &cq_);

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
                const quantra::PriceCapFloorResponse *response = call->reply.GetRoot();
                
                auto cap_floors = response->cap_floors();
                if (cap_floors && cap_floors->size() > 0)
                {
                    auto cap_floor = cap_floors->Get(0);
                    
                    std::cout << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << "          CAP PRICING RESULTS           " << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << std::fixed << std::setprecision(2);
                    std::cout << "NPV:              " << std::setw(15) << cap_floor->npv() << " EUR" << std::endl;
                    std::cout << std::fixed << std::setprecision(6);
                    std::cout << "ATM Rate:         " << std::setw(15) << cap_floor->atm_rate() * 100 << " %" << std::endl;
                    std::cout << "========================================" << std::endl;

                    // Print caplet details if available
                    auto caplets = cap_floor->cap_floor_lets();
                    if (caplets && caplets->size() > 0)
                    {
                        std::cout << std::endl;
                        std::cout << "CAPLET DETAILS:" << std::endl;
                        std::cout << "------------------------------------------------------------" << std::endl;
                        std::cout << std::setw(12) << "Fixing" 
                                  << std::setw(12) << "Payment"
                                  << std::setw(10) << "Strike"
                                  << std::setw(10) << "Forward"
                                  << std::setw(10) << "Discount" << std::endl;
                        std::cout << "------------------------------------------------------------" << std::endl;
                        
                        for (size_t i = 0; i < caplets->size(); i++)
                        {
                            auto caplet = caplets->Get(i);
                            std::cout << std::setw(12) << caplet->fixing_date()->str()
                                      << std::setw(12) << caplet->payment_date()->str()
                                      << std::setw(9) << std::fixed << std::setprecision(2) << caplet->strike() * 100 << "%"
                                      << std::setw(9) << std::fixed << std::setprecision(2) << caplet->forward_rate() * 100 << "%"
                                      << std::setw(10) << std::fixed << std::setprecision(6) << caplet->discount()
                                      << std::endl;
                        }
                    }
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
        flatbuffers::grpc::Message<quantra::PriceCapFloorResponse> reply;
        grpc::ClientContext context;
        grpc::Status status;

        std::unique_ptr<grpc::ClientAsyncResponseReader<
            flatbuffers::grpc::Message<quantra::PriceCapFloorResponse>>> response_reader;
    };

    std::unique_ptr<quantra::QuantraServer::Stub> stub_;
    grpc::CompletionQueue cq_;
};

int main(int argc, char **argv)
{
    std::cout << "========================================" << std::endl;
    std::cout << "      Cap/Floor Pricing Client          " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Cap Details:" << std::endl;
    std::cout << "  Type:        Cap" << std::endl;
    std::cout << "  Notional:    10,000,000 EUR" << std::endl;
    std::cout << "  Strike:      4.00%" << std::endl;
    std::cout << "  Maturity:    3 years" << std::endl;
    std::cout << "  Index:       3M Euribor" << std::endl;
    std::cout << "  Volatility:  20% (Black)" << std::endl;
    std::cout << std::endl;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    CapFloorClient client(grpc::CreateChannel(
        "localhost:50051", grpc::InsecureChannelCredentials()));

    int n = 1;

    if (argc > 1)
    {
        std::istringstream iss(argv[1]);
        iss >> n;
    }

    std::thread thread_ = std::thread(&CapFloorClient::AsyncCompleteRpc, &client, n);

    for (int i = 0; i < n; i++)
    {
        client.PriceCapFloor();
    }

    thread_.join();

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() 
              << "[ms]" << std::endl;

    return 0;
}
