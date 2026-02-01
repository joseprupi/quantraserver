#include "quantra_call.h"

std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceFixedRateBondData::JSONParser_(std::string json_str)
{
    return this->json_parser->PriceFixedRateBondRequestToFBS(json_str);
}

flatbuffers::Offset<quantra::PriceFixedRateBondRequest> PriceFixedRateBondData::QuantraToFBS(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
                                                                                             std::shared_ptr<structs::PriceFixedRateBondRequest> request_struct)
{
    return price_fixed_rate_bond_request_to_fbs(builder, request_struct);
}

void PriceFixedRateBondData::PrepareAsync(AsyncClientCall *call)
{
    call->response_reader =
        this->stub_->PrepareAsyncPriceFixedRateBond(&call->context, request_msg, &this->cq_);
}

std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFixedRateBondValues>>> PriceFixedRateBondData::FBSToQuantra(const quantra::PriceFixedRateBondResponse *response)
{
    return price_fixed_rate_bond_response_to_quantra(response);
}

std::shared_ptr<std::string> PriceFixedRateBondData::ResponseToJson(const uint8_t *buffer)
{
    return this->json_parser->PriceFixedRateBondResponseToJSON(buffer);
}

/* Floating Bond */

std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceFloatingRateBondData::JSONParser_(std::string json_str)
{
    return this->json_parser->PriceFloatingRateBondRequestToFBS(json_str);
}

flatbuffers::Offset<quantra::PriceFloatingRateBondRequest> PriceFloatingRateBondData::QuantraToFBS(std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
                                                                                                   std::shared_ptr<structs::PriceFloatingRateBondRequest> request_struct)
{
    return price_floating_rate_bond_request_to_fbs(builder, request_struct);
}

void PriceFloatingRateBondData::PrepareAsync(AsyncClientCall *call)
{
    call->response_reader =
        this->stub_->PrepareAsyncPriceFloatingRateBond(&call->context, request_msg, &this->cq_);
}

std::shared_ptr<std::vector<std::shared_ptr<structs::PriceFloatingRateBondValues>>> PriceFloatingRateBondData::FBSToQuantra(const quantra::PriceFloatingRateBondResponse *response)
{
    return price_floating_rate_bond_response_to_quantra(response);
}

std::shared_ptr<std::string> PriceFloatingRateBondData::ResponseToJson(const uint8_t *buffer)
{
    return this->json_parser->PriceFloatingRateBondResponseToJSON(buffer);
}