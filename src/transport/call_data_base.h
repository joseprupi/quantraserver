#ifndef QUANTRASERVER_CALL_DATA_BASE_H
#define QUANTRASERVER_CALL_DATA_BASE_H

#include <grpcpp/grpcpp.h>
#include <chrono>
#include <cstddef>
#include <exception>
#include <iostream>
#include <map>
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

namespace quantra { namespace transport {

// gRPC trailers (which carry the status message) are size-capped well below this;
// keep the real cause text but never blow the trailer budget.
constexpr std::size_t kMaxStatusMessageLen = 4096;

// Build the gRPC status message from an exception's text: carry the REAL cause to
// the caller, capping over-long text so the underlying reason still survives.
inline std::string ErrorStatusMessage(const char *what)
{
    std::string msg = (what != nullptr) ? what : "";
    if (msg.size() > kMaxStatusMessageLen)
    {
        msg.resize(kMaxStatusMessageLen);
        msg += " ...[truncated]";
    }
    return msg;
}

// Extract the caller's request id from inbound gRPC metadata for log tagging.
// Returns "-" when `x-request-id` is absent so logs grep uniformly.
inline std::string RequestId(
    const std::multimap<grpc::string_ref, grpc::string_ref> &metadata)
{
    auto it = metadata.find(grpc::string_ref("x-request-id"));
    if (it != metadata.end())
        return std::string(it->second.data(), it->second.size());
    return "-";
}

}} // namespace quantra::transport

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

            const std::string request_id =
                quantra::transport::RequestId(ctx_.client_metadata());

            std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder =
                std::make_shared<flatbuffers::grpc::MessageBuilder>();
            try
            {
                // Spawn the handler for the NEXT request first, so the deadline
                // short-circuit below still leaves the service accepting calls.
                this->CreateService(service_, cq_);

                // The caller's per-RPC deadline is propagated to the server
                // context. If it has already elapsed, the caller has timed out
                // and will discard whatever we send, so don't spend the
                // single-threaded worker computing a reply nobody will read.
                //
                // We key off the propagated deadline rather than
                // ctx_.IsCancelled(): on this async service IsCancelled() is
                // only safe once the AsyncNotifyWhenDone tag has been delivered
                // (RPC end), which this server does not register, and calling it
                // earlier is undefined. deadline() is always safe and captures
                // the exact signal the gateway's per-RPC deadline produces. A
                // caller with no deadline yields time_point::max(), so this is a
                // no-op for them.
                if (std::chrono::system_clock::now() >= ctx_.deadline())
                {
                    std::cerr << "[grpc] request deadline already elapsed before processing, skipping"
                              << " type=" << typeid(Message).name()
                              << " peer=" << ctx_.peer()
                              << " request_id=" << request_id
                              << std::endl;
                    status_ = FINISH;
                    responder_.FinishWithError(
                        grpc::Status(grpc::StatusCode::CANCELLED,
                                     "client cancelled or deadline expired"),
                        this);
                    return;
                }

                if (!request_msg.Verify())
                {
                    std::cerr << "[grpc] malformed FlatBuffer request"
                              << " type=" << typeid(Message).name()
                              << " peer=" << ctx_.peer()
                              << " bytes=" << request_msg.size()
                              << " request_id=" << request_id
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

                // The computation may have taken longer than the caller was
                // willing to wait: re-check the propagated deadline before
                // serializing/sending a reply nobody will read.
                if (std::chrono::system_clock::now() >= ctx_.deadline())
                {
                    std::cerr << "[grpc] request deadline elapsed during processing, dropping reply"
                              << " type=" << typeid(Message).name()
                              << " peer=" << ctx_.peer()
                              << " request_id=" << request_id
                              << std::endl;
                    status_ = FINISH;
                    responder_.FinishWithError(
                        grpc::Status(grpc::StatusCode::CANCELLED,
                                     "client cancelled or deadline expired"),
                        this);
                    return;
                }

                reply_ = builder->ReleaseMessage<Response>();
                if (!reply_.Verify())
                {
                    std::cerr << "[grpc] refusing to send invalid FlatBuffer reply"
                              << " request_type=" << typeid(Message).name()
                              << " response_type=" << typeid(Response).name()
                              << " peer=" << ctx_.peer()
                              << " bytes=" << reply_.size()
                              << " request_id=" << request_id
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
                          << " request_id=" << request_id
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::ABORTED, quantra::transport::ErrorStatusMessage(e.what()));
                responder_.FinishWithError(status, this);
            }
            catch (QuantraNotFound &e)
            {
                std::cerr << "[grpc] Quantra not-found"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " error=" << e.what()
                          << " request_id=" << request_id
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
                          << " request_id=" << request_id
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
                          << " request_id=" << request_id
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
                          << " request_id=" << request_id
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::ABORTED, quantra::transport::ErrorStatusMessage(e.what()));
                responder_.FinishWithError(status, this);
            }
            catch (std::exception &e)
            {
                std::cerr << "[grpc] std::exception while handling request"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " error=" << e.what()
                          << " request_id=" << request_id
                          << std::endl;
                status_ = FINISH;
                auto status = grpc::Status(grpc::StatusCode::ABORTED, quantra::transport::ErrorStatusMessage(e.what()));
                responder_.FinishWithError(status, this);
            }
            catch (...)
            {
                std::cerr << "[grpc] non-std exception while handling request"
                          << " type=" << typeid(Message).name()
                          << " peer=" << ctx_.peer()
                          << " request_id=" << request_id
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