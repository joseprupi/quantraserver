#ifndef QUANTRA_CALL_H
#define QUANTRA_CALL_H

#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include <grpc/support/log.h>
#include <thread>

#include "quantra_client.h"
#include "quantra_to_fbs.h"
#include "json_to_fbs.h"
#include "fbs_to_quantra.h"
#include "quantraserver.grpc.fb.h"
#include "fixed_rate_bond_response_generated.h"
#include "floating_rate_bond_response_generated.h"

class QuantraClient;
struct AsyncClientCall;

template <class RequestStruct, class Request, class ResponseStruct, class Response>
class QuantraCall
{
public:
    struct AsyncClientCall
    {

        flatbuffers::grpc::Message<Response> reply;
        flatbuffers::grpc::Message<quantra::Error> error_details;
        grpc::ClientContext context;
        grpc::Status status;
        int request_pos;
        std::unique_ptr<grpc::ClientAsyncResponseReader<flatbuffers::grpc::Message<Response>>> response_reader;
        bool json = false;
    };

    QuantraCall(std::shared_ptr<quantra::QuantraServer::Stub> stub_)
    {
        this->stub_ = stub_;
    }

    void AsyncCompleteRpc(int request_size)
    {
        void *got_tag;
        bool ok = false;

        int req_count = 0;

        while (this->cq_.Next(&got_tag, &ok))
        {
            AsyncClientCall *call = static_cast<AsyncClientCall *>(got_tag);

            GPR_ASSERT(ok);

            int position = call->request_pos;
            if (call->status.ok())
            {
                const Response *response = call->reply.GetRoot();

                if (call->json)
                {
                    json_response response;
                    response.response_value = this->ResponseToJson(call->reply.data());
                    response.ok = true;
                    this->json_responses[position] = std::make_shared<json_response>(response);
                }
                else
                {
                    auto quantra_response = this->FBSToQuantra(response);
                    this->responses.at(position) = quantra_response;
                }
            }
            else
            {
                std::string error_message = "Error when calling RPC method. \n";
                error_message.append("RPC code: " + std::to_string(call->status.error_code()) + '\n');
                error_message.append("RPC message: " + call->status.error_message() + '\n');
                error_message.append("Error details: " + call->status.error_details() + '\n');

                if (call->json)
                {
                    json_response response;
                    response.response_value = std::make_shared<std::string>(error_message);
                    response.ok = false;
                    this->json_responses[position] = std::make_shared<json_response>(response);
                }
            }

            delete call;

            req_count += 1;
            if (req_count == request_size)
                break;
        }
    }

    virtual void PrepareAsync(AsyncClientCall *call) = 0;
    virtual std::shared_ptr<flatbuffers::grpc::MessageBuilder> JSONParser_(std::string json_str) = 0;
    virtual flatbuffers::Offset<Request> QuantraToFBS(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder, std::shared_ptr<RequestStruct> struct_request) = 0;
    virtual std::shared_ptr<std::vector<std::shared_ptr<ResponseStruct>>> FBSToQuantra(const Response *response) = 0;
    virtual std::shared_ptr<std::string> ResponseToJson(const uint8_t *buffer) = 0;

    void Call(std::shared_ptr<RequestStruct> request, int request_tag)
    {
        // this->call = new AsyncClientCall;
        AsyncClientCall *call = new AsyncClientCall;
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = std::make_shared<flatbuffers::grpc::MessageBuilder>();
        auto bond_request = this->QuantraToFBS(builder, request);
        builder->Finish(bond_request);
        uint8_t *buf = builder->GetBufferPointer();
        int size = builder->GetSize();
        request_msg = builder->ReleaseMessage<Request>();
        call->request_pos = request_tag;
        this->PrepareAsync(call);
        call->response_reader->StartCall();
        call->response_reader->Finish(&call->reply, &call->status, (void *)call);
    }

    std::vector<std::shared_ptr<std::vector<std::shared_ptr<ResponseStruct>>>>
    RequestCall(std::vector<std::shared_ptr<RequestStruct>> request)
    {
        this->responses.resize(request.size());
        std::thread thread_ = std::thread(&QuantraCall::AsyncCompleteRpc, this, request.size());

        int request_tag = 0;
        for (auto it = request.begin(); it != request.end(); it++)
        {
            this->Call(*it, request_tag);
            request_tag += 1;
        }

        thread_.join();
        return this->responses;
    }

    std::shared_ptr<json_response> JSONRequest(std::string json)
    {
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder = this->JSONParser_(json);
        this->json_responses.resize(1);
        std::thread thread_ = std::thread(&QuantraCall::AsyncCompleteRpc, this, 1);
        request_msg = builder->ReleaseMessage<Request>();

        AsyncClientCall *call = new AsyncClientCall;
        call->json = true;
        call->request_pos = 0;

        this->PrepareAsync(call);

        call->response_reader->StartCall();

        call->response_reader->Finish(&call->reply, &call->status, (void *)call);

        thread_.join();

        return this->json_responses[0];
    }

    grpc::CompletionQueue cq_;

protected:
    std::shared_ptr<quantra::QuantraServer::Stub> stub_;
    std::shared_ptr<JSONParser> json_parser = std::make_shared<JSONParser>();
    flatbuffers::grpc::Message<Request> request_msg;
    std::vector<std::shared_ptr<std::vector<std::shared_ptr<ResponseStruct>>>> responses;
    std::vector<std::shared_ptr<json_response>> json_responses;
};

class PriceFixedRateBondData : public QuantraCall<structs::PriceFixedRateBondRequest,
                                                  quantra::PriceFixedRateBondRequest,
                                                  structs::PriceFixedRateBondValues,
                                                  quantra::PriceFixedRateBondResponse>
{

public:
    PriceFixedRateBondData(std::shared_ptr<quantra::QuantraServer::Stub> stub_) : QuantraCall(stub_){};
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> JSONParser_(std::string json_str);
    flatbuffers::Offset<quantra::PriceFixedRateBondRequest> QuantraToFBS(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
                                                                         std::shared_ptr<structs::PriceFixedRateBondRequest> request_struct);
    void PrepareAsync(AsyncClientCall *call);
    std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFixedRateBondValues>>> FBSToQuantra(const quantra::PriceFixedRateBondResponse *response);
    virtual std::shared_ptr<std::string> ResponseToJson(const uint8_t *buffer);
};

class PriceFloatingRateBondData : public QuantraCall<structs::PriceFloatingRateBondRequest,
                                                     quantra::PriceFloatingRateBondRequest,
                                                     structs::PriceFloatingRateBondValues,
                                                     quantra::PriceFloatingRateBondResponse>
{

public:
    PriceFloatingRateBondData(std::shared_ptr<quantra::QuantraServer::Stub> stub_) : QuantraCall(stub_){};
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> JSONParser_(std::string json_str);
    flatbuffers::Offset<quantra::PriceFloatingRateBondRequest> QuantraToFBS(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
                                                                            std::shared_ptr<structs::PriceFloatingRateBondRequest> request_struct);
    void PrepareAsync(AsyncClientCall *call);
    std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFloatingRateBondValues>>> FBSToQuantra(const quantra::PriceFloatingRateBondResponse *response);
    virtual std::shared_ptr<std::string> ResponseToJson(const uint8_t *buffer);
};

#endif