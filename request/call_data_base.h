#ifndef QUANTRASERVER_CALL_DATA_BASE_H
#define QUANTRASERVER_CALL_DATA_BASE_H

#include <grpcpp/grpcpp.h>
#include <iostream>
#include <memory>
#include <cassert>

#include <ql/quantlib.hpp>

#include "flatbuffers/grpc.h"
#include "quantraserver.grpc.fb.h"
#include "quantraserver_generated.h"
#include "error.h"

// Use quantra namespace for QuantraServer
using quantra::QuantraServer;

/**
 * CallData - Base class for all async handlers.
 */
class CallData
{
public:
    virtual ~CallData() = default;
    virtual void Proceed() = 0;
};

/**
 * CallDataGeneric - Template base class that handles the gRPC async machinery.
 */
template <class Message, class Request, class Response, class ResponseBuilder>
class CallDataGeneric : public CallData
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

    void Proceed() override
    {
        if (status_ == CREATE)
        {
            status_ = PROCESS;
            this->RequestCall();
        }
        else if (status_ == PROCESS)
        {
            std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = 
                std::make_shared<flatbuffers::grpc::MessageBuilder>();
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
            catch (QuantLib::Error &e)
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
            catch (std::exception &e)
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

#endif // QUANTRASERVER_CALL_DATA_BASE_H