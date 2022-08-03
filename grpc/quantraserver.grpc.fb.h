// Generated by the gRPC C++ plugin.
// If you make any local change, they will be lost.
// source: quantraserver
#ifndef GRPC_quantraserver__INCLUDED
#define GRPC_quantraserver__INCLUDED

#include "quantraserver_generated.h"
#include "flatbuffers/grpc.h"

#include <grpcpp/impl/codegen/async_stream.h>
#include <grpcpp/impl/codegen/async_unary_call.h>
#include <grpcpp/impl/codegen/method_handler.h>
#include <grpcpp/impl/codegen/proto_utils.h>
#include <grpcpp/impl/codegen/rpc_method.h>
#include <grpcpp/impl/codegen/service_type.h>
#include <grpcpp/impl/codegen/status.h>
#include <grpcpp/impl/codegen/stub_options.h>
#include <grpcpp/impl/codegen/sync_stream.h>

namespace grpc {
class CompletionQueue;
class Channel;
class ServerCompletionQueue;
class ServerContext;
}  // namespace grpc

namespace quantra {

class QuantraServer final {
 public:
  static constexpr char const* service_full_name() {
    return "quantra.QuantraServer";
  }
  class StubInterface {
   public:
    virtual ~StubInterface() {}
    virtual ::grpc::Status PriceFixedRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, flatbuffers::grpc::Message<PriceFixedRateBondResponse>* response) = 0;
    std::unique_ptr< ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>> AsyncPriceFixedRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, ::grpc::CompletionQueue* cq) {
      return std::unique_ptr< ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>>(AsyncPriceFixedRateBondRaw(context, request, cq));
    }
    std::unique_ptr< ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>> PrepareAsyncPriceFixedRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, ::grpc::CompletionQueue* cq) {
      return std::unique_ptr< ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>>(PrepareAsyncPriceFixedRateBondRaw(context, request, cq));
    }
    virtual ::grpc::Status PriceFloatingRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, flatbuffers::grpc::Message<PriceFloatingRateBondResponse>* response) = 0;
    std::unique_ptr< ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>> AsyncPriceFloatingRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, ::grpc::CompletionQueue* cq) {
      return std::unique_ptr< ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>>(AsyncPriceFloatingRateBondRaw(context, request, cq));
    }
    std::unique_ptr< ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>> PrepareAsyncPriceFloatingRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, ::grpc::CompletionQueue* cq) {
      return std::unique_ptr< ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>>(PrepareAsyncPriceFloatingRateBondRaw(context, request, cq));
    }
  private:
    virtual ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>* AsyncPriceFixedRateBondRaw(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, ::grpc::CompletionQueue* cq) = 0;
    virtual ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>* PrepareAsyncPriceFixedRateBondRaw(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, ::grpc::CompletionQueue* cq) = 0;
    virtual ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>* AsyncPriceFloatingRateBondRaw(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, ::grpc::CompletionQueue* cq) = 0;
    virtual ::grpc::ClientAsyncResponseReaderInterface< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>* PrepareAsyncPriceFloatingRateBondRaw(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, ::grpc::CompletionQueue* cq) = 0;
  };
  class Stub final : public StubInterface {
   public:
    Stub(const std::shared_ptr< ::grpc::ChannelInterface>& channel);
    ::grpc::Status PriceFixedRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, flatbuffers::grpc::Message<PriceFixedRateBondResponse>* response) override;
    std::unique_ptr< ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>> AsyncPriceFixedRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, ::grpc::CompletionQueue* cq) {
      return std::unique_ptr< ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>>(AsyncPriceFixedRateBondRaw(context, request, cq));
    }
    std::unique_ptr< ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>> PrepareAsyncPriceFixedRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, ::grpc::CompletionQueue* cq) {
      return std::unique_ptr< ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>>(PrepareAsyncPriceFixedRateBondRaw(context, request, cq));
    }
    ::grpc::Status PriceFloatingRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, flatbuffers::grpc::Message<PriceFloatingRateBondResponse>* response) override;
    std::unique_ptr< ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>> AsyncPriceFloatingRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, ::grpc::CompletionQueue* cq) {
      return std::unique_ptr< ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>>(AsyncPriceFloatingRateBondRaw(context, request, cq));
    }
    std::unique_ptr< ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>> PrepareAsyncPriceFloatingRateBond(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, ::grpc::CompletionQueue* cq) {
      return std::unique_ptr< ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>>(PrepareAsyncPriceFloatingRateBondRaw(context, request, cq));
    }
  
   private:
    std::shared_ptr< ::grpc::ChannelInterface> channel_;
    ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>* AsyncPriceFixedRateBondRaw(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, ::grpc::CompletionQueue* cq) override;
    ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>* PrepareAsyncPriceFixedRateBondRaw(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& request, ::grpc::CompletionQueue* cq) override;
    ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>* AsyncPriceFloatingRateBondRaw(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, ::grpc::CompletionQueue* cq) override;
    ::grpc::ClientAsyncResponseReader< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>* PrepareAsyncPriceFloatingRateBondRaw(::grpc::ClientContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& request, ::grpc::CompletionQueue* cq) override;
    const ::grpc::internal::RpcMethod rpcmethod_PriceFixedRateBond_;
    const ::grpc::internal::RpcMethod rpcmethod_PriceFloatingRateBond_;
  };
  static std::unique_ptr<Stub> NewStub(const std::shared_ptr< ::grpc::ChannelInterface>& channel, const ::grpc::StubOptions& options = ::grpc::StubOptions());
  
  class Service : public ::grpc::Service {
   public:
    Service();
    virtual ~Service();
    virtual ::grpc::Status PriceFixedRateBond(::grpc::ServerContext* context, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>* request, flatbuffers::grpc::Message<PriceFixedRateBondResponse>* response);
    virtual ::grpc::Status PriceFloatingRateBond(::grpc::ServerContext* context, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>* request, flatbuffers::grpc::Message<PriceFloatingRateBondResponse>* response);
  };
  template <class BaseClass>
  class WithAsyncMethod_PriceFixedRateBond : public BaseClass {
   private:
    void BaseClassMustBeDerivedFromService(const Service */*service*/) {}
   public:
    WithAsyncMethod_PriceFixedRateBond() {
      ::grpc::Service::MarkMethodAsync(0);
    }
    ~WithAsyncMethod_PriceFixedRateBond() override {
      BaseClassMustBeDerivedFromService(this);
    }
    // disable synchronous version of this method
    ::grpc::Status PriceFixedRateBond(::grpc::ServerContext* /*context*/, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>* /*request*/, flatbuffers::grpc::Message<PriceFixedRateBondResponse>* /*response*/) final override {
      abort();
      return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
    }
    void RequestPriceFixedRateBond(::grpc::ServerContext* context, flatbuffers::grpc::Message<PriceFixedRateBondRequest>* request, ::grpc::ServerAsyncResponseWriter< flatbuffers::grpc::Message<PriceFixedRateBondResponse>>* response, ::grpc::CompletionQueue* new_call_cq, ::grpc::ServerCompletionQueue* notification_cq, void *tag) {
      ::grpc::Service::RequestAsyncUnary(0, context, request, response, new_call_cq, notification_cq, tag);
    }
  };
  template <class BaseClass>
  class WithAsyncMethod_PriceFloatingRateBond : public BaseClass {
   private:
    void BaseClassMustBeDerivedFromService(const Service */*service*/) {}
   public:
    WithAsyncMethod_PriceFloatingRateBond() {
      ::grpc::Service::MarkMethodAsync(1);
    }
    ~WithAsyncMethod_PriceFloatingRateBond() override {
      BaseClassMustBeDerivedFromService(this);
    }
    // disable synchronous version of this method
    ::grpc::Status PriceFloatingRateBond(::grpc::ServerContext* /*context*/, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>* /*request*/, flatbuffers::grpc::Message<PriceFloatingRateBondResponse>* /*response*/) final override {
      abort();
      return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
    }
    void RequestPriceFloatingRateBond(::grpc::ServerContext* context, flatbuffers::grpc::Message<PriceFloatingRateBondRequest>* request, ::grpc::ServerAsyncResponseWriter< flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>* response, ::grpc::CompletionQueue* new_call_cq, ::grpc::ServerCompletionQueue* notification_cq, void *tag) {
      ::grpc::Service::RequestAsyncUnary(1, context, request, response, new_call_cq, notification_cq, tag);
    }
  };
  typedef   WithAsyncMethod_PriceFixedRateBond<  WithAsyncMethod_PriceFloatingRateBond<  Service   >   >   AsyncService;
  template <class BaseClass>
  class WithGenericMethod_PriceFixedRateBond : public BaseClass {
   private:
    void BaseClassMustBeDerivedFromService(const Service */*service*/) {}
   public:
    WithGenericMethod_PriceFixedRateBond() {
      ::grpc::Service::MarkMethodGeneric(0);
    }
    ~WithGenericMethod_PriceFixedRateBond() override {
      BaseClassMustBeDerivedFromService(this);
    }
    // disable synchronous version of this method
    ::grpc::Status PriceFixedRateBond(::grpc::ServerContext* /*context*/, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>* /*request*/, flatbuffers::grpc::Message<PriceFixedRateBondResponse>* /*response*/) final override {
      abort();
      return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
    }
  };
  template <class BaseClass>
  class WithGenericMethod_PriceFloatingRateBond : public BaseClass {
   private:
    void BaseClassMustBeDerivedFromService(const Service */*service*/) {}
   public:
    WithGenericMethod_PriceFloatingRateBond() {
      ::grpc::Service::MarkMethodGeneric(1);
    }
    ~WithGenericMethod_PriceFloatingRateBond() override {
      BaseClassMustBeDerivedFromService(this);
    }
    // disable synchronous version of this method
    ::grpc::Status PriceFloatingRateBond(::grpc::ServerContext* /*context*/, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>* /*request*/, flatbuffers::grpc::Message<PriceFloatingRateBondResponse>* /*response*/) final override {
      abort();
      return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
    }
  };
  template <class BaseClass>
  class WithStreamedUnaryMethod_PriceFixedRateBond : public BaseClass {
   private:
    void BaseClassMustBeDerivedFromService(const Service */*service*/) {}
   public:
    WithStreamedUnaryMethod_PriceFixedRateBond() {
      ::grpc::Service::MarkMethodStreamed(0,
        new ::grpc::internal::StreamedUnaryHandler< flatbuffers::grpc::Message<PriceFixedRateBondRequest>, flatbuffers::grpc::Message<PriceFixedRateBondResponse>>(std::bind(&WithStreamedUnaryMethod_PriceFixedRateBond<BaseClass>::StreamedPriceFixedRateBond, this, std::placeholders::_1, std::placeholders::_2)));
    }
    ~WithStreamedUnaryMethod_PriceFixedRateBond() override {
      BaseClassMustBeDerivedFromService(this);
    }
    // disable regular version of this method
    ::grpc::Status PriceFixedRateBond(::grpc::ServerContext* /*context*/, const flatbuffers::grpc::Message<PriceFixedRateBondRequest>* /*request*/, flatbuffers::grpc::Message<PriceFixedRateBondResponse>* /*response*/) final override {
      abort();
      return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
    }
    // replace default version of method with streamed unary
    virtual ::grpc::Status StreamedPriceFixedRateBond(::grpc::ServerContext* context, ::grpc::ServerUnaryStreamer< flatbuffers::grpc::Message<PriceFixedRateBondRequest>,flatbuffers::grpc::Message<PriceFixedRateBondResponse>>* server_unary_streamer) = 0;
  };
  template <class BaseClass>
  class WithStreamedUnaryMethod_PriceFloatingRateBond : public BaseClass {
   private:
    void BaseClassMustBeDerivedFromService(const Service */*service*/) {}
   public:
    WithStreamedUnaryMethod_PriceFloatingRateBond() {
      ::grpc::Service::MarkMethodStreamed(1,
        new ::grpc::internal::StreamedUnaryHandler< flatbuffers::grpc::Message<PriceFloatingRateBondRequest>, flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>(std::bind(&WithStreamedUnaryMethod_PriceFloatingRateBond<BaseClass>::StreamedPriceFloatingRateBond, this, std::placeholders::_1, std::placeholders::_2)));
    }
    ~WithStreamedUnaryMethod_PriceFloatingRateBond() override {
      BaseClassMustBeDerivedFromService(this);
    }
    // disable regular version of this method
    ::grpc::Status PriceFloatingRateBond(::grpc::ServerContext* /*context*/, const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>* /*request*/, flatbuffers::grpc::Message<PriceFloatingRateBondResponse>* /*response*/) final override {
      abort();
      return ::grpc::Status(::grpc::StatusCode::UNIMPLEMENTED, "");
    }
    // replace default version of method with streamed unary
    virtual ::grpc::Status StreamedPriceFloatingRateBond(::grpc::ServerContext* context, ::grpc::ServerUnaryStreamer< flatbuffers::grpc::Message<PriceFloatingRateBondRequest>,flatbuffers::grpc::Message<PriceFloatingRateBondResponse>>* server_unary_streamer) = 0;
  };
  typedef   WithStreamedUnaryMethod_PriceFixedRateBond<  WithStreamedUnaryMethod_PriceFloatingRateBond<  Service   >   >   StreamedUnaryService;
  typedef   Service   SplitStreamedService;
  typedef   WithStreamedUnaryMethod_PriceFixedRateBond<  WithStreamedUnaryMethod_PriceFloatingRateBond<  Service   >   >   StreamedService;
};

}  // namespace quantra


#endif  // GRPC_quantraserver__INCLUDED