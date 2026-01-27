#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <chrono>
#include <iomanip>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <thread>

#include "data/vanilla_swap_request_fbs.h"

using grpc::Channel;
using grpc::ClientAsyncResponseReader;
using grpc::ClientContext;
using grpc::CompletionQueue;
using grpc::Status;

class SwapClient
{
public:
    explicit SwapClient(std::shared_ptr<grpc::Channel> channel)
        : stub_(quantra::QuantraServer::NewStub(channel)) {}

    void PriceSwap()
    {
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = 
            std::make_shared<flatbuffers::grpc::MessageBuilder>();
        swap_request_fbs(builder);

        auto request_msg = builder->ReleaseMessage<quantra::PriceVanillaSwapRequest>();

        AsyncClientCall *call = new AsyncClientCall;

        call->response_reader =
            stub_->PrepareAsyncPriceVanillaSwap(&call->context, request_msg, &cq_);

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
                const quantra::PriceVanillaSwapResponse *response = call->reply.GetRoot();
                
                auto swaps = response->swaps();
                if (swaps && swaps->size() > 0)
                {
                    auto swap = swaps->Get(0);
                    
                    std::cout << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << "         VANILLA SWAP RESULTS           " << std::endl;
                    std::cout << "========================================" << std::endl;
                    std::cout << std::fixed << std::setprecision(2);
                    std::cout << "NPV:              " << std::setw(15) << swap->npv() << " EUR" << std::endl;
                    std::cout << std::fixed << std::setprecision(6);
                    std::cout << "Fair Rate:        " << std::setw(15) << swap->fair_rate() * 100 << " %" << std::endl;
                    std::cout << "Fair Spread:      " << std::setw(15) << swap->fair_spread() * 10000 << " bps" << std::endl;
                    std::cout << std::fixed << std::setprecision(2);
                    std::cout << "Fixed Leg NPV:    " << std::setw(15) << swap->fixed_leg_npv() << " EUR" << std::endl;
                    std::cout << "Floating Leg NPV: " << std::setw(15) << swap->floating_leg_npv() << " EUR" << std::endl;
                    std::cout << "Fixed Leg BPS:    " << std::setw(15) << swap->fixed_leg_bps() << " EUR" << std::endl;
                    std::cout << "Floating Leg BPS: " << std::setw(15) << swap->floating_leg_bps() << " EUR" << std::endl;
                    std::cout << "========================================" << std::endl;

                    // Print fixed leg flows
                    auto fixed_flows = swap->fixed_leg_flows();
                    if (fixed_flows && fixed_flows->size() > 0)
                    {
                        std::cout << std::endl;
                        std::cout << "FIXED LEG CASHFLOWS:" << std::endl;
                        std::cout << "------------------------------------------------------------" << std::endl;
                        std::cout << std::setw(12) << "Payment" 
                                  << std::setw(15) << "Amount" 
                                  << std::setw(10) << "Rate"
                                  << std::setw(15) << "PV" << std::endl;
                        std::cout << "------------------------------------------------------------" << std::endl;
                        
                        for (size_t i = 0; i < fixed_flows->size(); i++)
                        {
                            auto flow = fixed_flows->Get(i);
                            std::cout << std::setw(12) << flow->payment_date()->str()
                                      << std::setw(15) << std::fixed << std::setprecision(2) << flow->amount()
                                      << std::setw(9) << std::fixed << std::setprecision(4) << flow->rate() * 100 << "%"
                                      << std::setw(15) << std::fixed << std::setprecision(2) << flow->present_value()
                                      << std::endl;
                        }
                    }

                    // Print floating leg flows
                    auto float_flows = swap->floating_leg_flows();
                    if (float_flows && float_flows->size() > 0)
                    {
                        std::cout << std::endl;
                        std::cout << "FLOATING LEG CASHFLOWS:" << std::endl;
                        std::cout << "------------------------------------------------------------" << std::endl;
                        std::cout << std::setw(12) << "Payment" 
                                  << std::setw(15) << "Amount" 
                                  << std::setw(10) << "Fixing"
                                  << std::setw(15) << "PV" << std::endl;
                        std::cout << "------------------------------------------------------------" << std::endl;
                        
                        for (size_t i = 0; i < float_flows->size(); i++)
                        {
                            auto flow = float_flows->Get(i);
                            std::cout << std::setw(12) << flow->payment_date()->str()
                                      << std::setw(15) << std::fixed << std::setprecision(2) << flow->amount()
                                      << std::setw(9) << std::fixed << std::setprecision(4) << flow->index_fixing() * 100 << "%"
                                      << std::setw(15) << std::fixed << std::setprecision(2) << flow->present_value()
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
        flatbuffers::grpc::Message<quantra::PriceVanillaSwapResponse> reply;
        grpc::ClientContext context;
        grpc::Status status;

        std::unique_ptr<grpc::ClientAsyncResponseReader<
            flatbuffers::grpc::Message<quantra::PriceVanillaSwapResponse>>> response_reader;
    };

    std::unique_ptr<quantra::QuantraServer::Stub> stub_;
    grpc::CompletionQueue cq_;
};

int main(int argc, char **argv)
{
    std::cout << "========================================" << std::endl;
    std::cout << "    Vanilla Swap Pricing Client         " << std::endl;
    std::cout << "========================================" << std::endl;
    std::cout << std::endl;
    std::cout << "Swap Details:" << std::endl;
    std::cout << "  Type:        Payer (pay fixed, receive floating)" << std::endl;
    std::cout << "  Notional:    10,000,000 EUR" << std::endl;
    std::cout << "  Fixed Rate:  3.50%" << std::endl;
    std::cout << "  Float Index: 6M Euribor" << std::endl;
    std::cout << "  Maturity:    5 years" << std::endl;
    std::cout << std::endl;

    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();

    SwapClient client(grpc::CreateChannel(
        "localhost:50051", grpc::InsecureChannelCredentials()));

    int n = 1;

    if (argc > 1)
    {
        std::istringstream iss(argv[1]);
        iss >> n;
    }

    std::thread thread_ = std::thread(&SwapClient::AsyncCompleteRpc, &client, n);

    for (int i = 0; i < n; i++)
    {
        client.PriceSwap();
    }

    thread_.join();

    std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();

    std::cout << "Time = " << std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count() 
              << "[ms]" << std::endl;

    return 0;
}