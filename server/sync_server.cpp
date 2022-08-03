#include "quantraserver.grpc.fb.h"
#include "quantraserver_generated.h"

#include "fixed_rate_bond_pricing_request.h"
#include "floating_rate_bond_pricing_request.h"
#include "price_fixed_rate_bond_request_generated.h"
#include "price_floating_rate_bond_request_generated.h"

#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>
#include <sstream>

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

        HandleRpcs();
    }

private:
    class CallData
    {
    public:
        virtual void Proceed() = 0;
    };

    template <class Message, class Request, class Response, class ResponseBuilder>
    class CallDataGeneric : CallData
    {
    public:
        explicit CallDataGeneric(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
            : service_(service), cq_(cq), responder_(&ctx_), status_(CREATE)
        {
        }

        void start()
        {
            Proceed();
        }

        void Proceed()
        {
            if (status_ == CREATE)
            {
                status_ = PROCESS;
                this->RequestCall();
            }
            else if (status_ == PROCESS)
            {
                std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
                try
                {
                    this->CreateService(service_, cq_);
                    Request request;
                    auto response = request.request(builder, request_msg.GetRoot());
                    builder->Finish(response);

                    reply_ = builder->ReleaseMessage<Response>();
                    assert(reply_.Verify());

                    status_ = FINISH;
                    responder_.Finish(reply_, grpc::Status::OK, this);
                }
                catch (QuantLib::Error e)
                {

                    std::string error_msg = "QuantLib error: ";
                    error_msg.append(e.what());

                    builder->Reset();
                    ResponseBuilder empty_pricing(*builder);
                    builder->Finish(empty_pricing.Finish());

                    reply_ = builder->ReleaseMessage<Response>();
                    assert(reply_.Verify());

                    status_ = FINISH;
                    auto status = grpc::Status(grpc::StatusCode::ABORTED, "QuantLib error", error_msg);
                    responder_.Finish(reply_, status, this);
                }
                catch (QuantraError &e)
                {

                    std::string error_msg = "Quantra error: ";
                    error_msg.append(e.what());
                    std::cout << "Quantra error:  " << error_msg << std::endl;

                    builder->Reset();
                    ResponseBuilder empty_pricing(*builder);
                    builder->Finish(empty_pricing.Finish());

                    reply_ = builder->ReleaseMessage<Response>();
                    assert(reply_.Verify());

                    status_ = FINISH;
                    auto status = grpc::Status(grpc::StatusCode::ABORTED, "Quantra error", error_msg);
                    responder_.Finish(reply_, status, this);
                }
                catch (std::exception e)
                {
                    std::string error_msg = "Unknown error: ";
                    error_msg.append(e.what());
                    std::cout << "Unknown error:  " << error_msg << std::endl;

                    builder->Reset();
                    ResponseBuilder empty_pricing(*builder);
                    builder->Finish(empty_pricing.Finish());

                    reply_ = builder->ReleaseMessage<Response>();
                    assert(reply_.Verify());

                    status_ = FINISH;
                    auto status = grpc::Status(grpc::StatusCode::ABORTED, "Unknown error", error_msg);
                    responder_.Finish(reply_, status, this);
                }
                catch (...)
                {

                    std::cout << "Unknown error exception:  " << std::endl;
                }
            }
            else
            {
                GPR_ASSERT(status_ == FINISH);
                delete this;
            }
        }

        virtual void RequestCall() = 0;
        virtual void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) = 0;

    protected:
        QuantraServer::AsyncService *service_;
        grpc::ServerCompletionQueue *cq_;
        grpc::ServerContext ctx_;

        flatbuffers::grpc::Message<Message> request_msg;
        flatbuffers::grpc::Message<Response> reply_;

        grpc::ServerAsyncResponseWriter<flatbuffers::grpc::Message<Response>> responder_;

        enum CallStatus
        {
            CREATE,
            PROCESS,
            FINISH
        };
        CallStatus status_;
    };

    class PriceFixedRateBondData : public CallDataGeneric<quantra::PriceFixedRateBondRequest, FixedRateBondPricingRequest, PriceFixedRateBondResponse, PriceFixedRateBondResponseBuilder>
    {
    public:
        PriceFixedRateBondData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
            : CallDataGeneric(service, cq)
        {
        }
        void RequestCall()
        {
            service_->RequestPriceFixedRateBond(&this->ctx_, &this->request_msg, &this->responder_, this->cq_, this->cq_, this);
        }
        void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        {
            auto price_fixed_bonds = new PriceFixedRateBondData(service_, cq_);
            price_fixed_bonds->start();
        }
    };

    class PriceFloatingRateBondData : public CallDataGeneric<quantra::PriceFloatingRateBondRequest, FloatingRateBondPricingRequest, PriceFloatingRateBondResponse, PriceFloatingRateBondResponseBuilder>
    {
    public:
        PriceFloatingRateBondData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
            : CallDataGeneric(service, cq)
        {
        }
        void RequestCall()
        {
            service_->RequestPriceFloatingRateBond(&this->ctx_, &this->request_msg, &this->responder_, this->cq_, this->cq_, this);
        }
        void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        {
            auto price_floating_bonds = new PriceFloatingRateBondData(service_, cq_);
            price_floating_bonds->start();
        }
    };

    void
    HandleRpcs()
    {

        void *tag;
        bool ok;

        auto price_fixed_bonds = new PriceFixedRateBondData(&service_, cq_.get());
        price_fixed_bonds->start();
        auto price_floating_bonds = new PriceFloatingRateBondData(&service_, cq_.get());
        price_floating_bonds->start();

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
    if (argc > 1)
    {
        std::string port(argv[1]);
        ServerImpl server;
        server.Run(port);
    }
    else
    {
        ServerImpl server;
        server.Run("50051");
    }

    return 0;
}