#ifndef QUANTRASERVER_CALL_DATA_BASE_H
#define QUANTRASERVER_CALL_DATA_BASE_H

#include <grpcpp/grpcpp.h>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <typeinfo>

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
    virtual void Proceed(bool ok) = 0;
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
        Proceed(true);
    }

    void Proceed(bool ok) override
    {
        if (status_ == CREATE)
        {
            status_ = PROCESS;
            this->RequestCall();
        }
        else if (status_ == PROCESS)
        {
            if (!ok)
            {
                delete this;
                return;
            }

            std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = 
                std::make_shared<flatbuffers::grpc::MessageBuilder>();
            try
            {
                this->CreateService(service_, cq_);

                if (!request_msg.Verify())
                {
                    std::cerr << "[grpc] malformed FlatBuffer request"
                              << " type=" << typeid(Message).name()
                              << " peer=" << ctx_.peer()
                              << " bytes=" << request_msg.size()
                              << std::endl;
                    status_ = FINISH;
                    responder_.FinishWithError(
                        grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Malformed request"),
                        this);
                    return;
                }

                Request request;
                auto response = request.request(builder, request_msg.GetRoot());
                builder->Finish(response);

                reply_ = builder->ReleaseMessage<Response>();
                if (!reply_.Verify())
                {
                    std::cerr << "[grpc] refusing to send invalid FlatBuffer reply"
                              << " request_type=" << typeid(Message).name()
                              << " response_type=" << typeid(Response).name()
                              << " peer=" << ctx_.peer()
                              << " bytes=" << reply_.size()
                              << std::endl;
                    status_ = FINISH;
                    responder_.FinishWithError(
                        grpc::Status(grpc::StatusCode::INTERNAL, "Invalid server response"),
                        this);
                    return;
                }

                status_ = FINISH;
                responder_.Finish(reply_, grpc::Status::OK, this);
            }
            catch (QuantLib::Error &e)
            {
                std::cerr << "[grpc] QuantLib exception"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " error=" << e.what()
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::ABORTED, "QuantLib error");
                responder_.FinishWithError(status, this);
            }
            catch (QuantraNotFound &e)
            {
                std::cerr << "[grpc] Quantra not-found"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " error=" << e.what()
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::NOT_FOUND, e.what());
                responder_.FinishWithError(status, this);
            }
            catch (QuantraInvalidArgument &e)
            {
                std::cerr << "[grpc] Quantra invalid-argument"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " error=" << e.what()
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what());
                responder_.FinishWithError(status, this);
            }
            catch (QuantraNotImplemented &e)
            {
                std::cerr << "[grpc] Quantra not-implemented"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " error=" << e.what()
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::UNIMPLEMENTED, e.what());
                responder_.FinishWithError(status, this);
            }
            catch (QuantraError &e)
            {
                std::cerr << "[grpc] Quantra exception"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " error=" << e.what()
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::ABORTED, "Quantra error");
                responder_.FinishWithError(status, this);
            }
            catch (std::exception &e)
            {
                std::cerr << "[grpc] std::exception while handling request"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " error=" << e.what()
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::ABORTED, "Unknown error");
                responder_.FinishWithError(status, this);
            }
            catch (...)
            {
                std::cerr << "[grpc] non-std exception while handling request"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::ABORTED, "Unknown error");
                responder_.FinishWithError(status, this);
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