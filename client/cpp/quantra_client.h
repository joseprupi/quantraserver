/**
 * @file quantra_client.h
 * @brief Quantra gRPC Client Library
 * 
 * Updated for FlatBuffers v25.x which renamed GenerateText -> GenText
 * and changed return type from bool to const char* (nullptr = success)
 */

#ifndef QUANTRA_CLIENT_H
#define QUANTRA_CLIENT_H

#include <iostream>
#include <memory>
#include <string>
#include <sstream>
#include <fstream>

#include <grpcpp/grpcpp.h>
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"
#include "flatbuffers/grpc.h"

#include "quantraserver.grpc.fb.h"

// Response types
#include "fixed_rate_bond_response_generated.h"
#include "floating_rate_bond_response_generated.h"
#include "vanilla_swap_response_generated.h"
#include "fra_response_generated.h"
#include "cap_floor_response_generated.h"
#include "swaption_response_generated.h"
#include "cds_response_generated.h"

namespace quantra {

// =============================================================================
// Schema Paths
// =============================================================================

#define FBS_INCLUDE_DIRECTORY "/workspace/flatbuffers/fbs"

// Request schemas
#define FIXED_RATE_BOND_REQUEST_PATH "/workspace/flatbuffers/fbs/price_fixed_rate_bond_request.fbs"
#define FLOATING_RATE_BOND_REQUEST_PATH "/workspace/flatbuffers/fbs/price_floating_rate_bond_request.fbs"
#define VANILLA_SWAP_REQUEST_PATH "/workspace/flatbuffers/fbs/price_vanilla_swap_request.fbs"
#define FRA_REQUEST_PATH "/workspace/flatbuffers/fbs/price_fra_request.fbs"
#define CAP_FLOOR_REQUEST_PATH "/workspace/flatbuffers/fbs/price_cap_floor_request.fbs"
#define SWAPTION_REQUEST_PATH "/workspace/flatbuffers/fbs/price_swaption_request.fbs"
#define CDS_REQUEST_PATH "/workspace/flatbuffers/fbs/price_cds_request.fbs"

// Response schemas
#define FIXED_RATE_BOND_RESPONSE_PATH "/workspace/flatbuffers/fbs/fixed_rate_bond_response.fbs"
#define FLOATING_RATE_BOND_RESPONSE_PATH "/workspace/flatbuffers/fbs/floating_rate_bond_response.fbs"
#define VANILLA_SWAP_RESPONSE_PATH "/workspace/flatbuffers/fbs/vanilla_swap_response.fbs"
#define FRA_RESPONSE_PATH "/workspace/flatbuffers/fbs/fra_response.fbs"
#define CAP_FLOOR_RESPONSE_PATH "/workspace/flatbuffers/fbs/cap_floor_response.fbs"
#define SWAPTION_RESPONSE_PATH "/workspace/flatbuffers/fbs/swaption_response.fbs"
#define CDS_RESPONSE_PATH "/workspace/flatbuffers/fbs/cds_response.fbs"

// =============================================================================
// JSON Response Structure
// =============================================================================

struct JsonResponse {
    bool ok;
    int status_code;
    std::string body;
    
    static JsonResponse Success(const std::string& json) {
        return {true, 200, json};
    }
    
    static JsonResponse BadRequest(const std::string& error) {
        return {false, 400, R"({"error": ")" + error + R"("})"};
    }
    
    static JsonResponse ServerError(const std::string& error) {
        return {false, 500, R"({"error": ")" + error + R"("})"};
    }
    
    static JsonResponse GrpcError(const grpc::Status& status) {
        std::ostringstream oss;
        oss << R"({"error": "gRPC error", "code": )" << status.error_code()
            << R"(, "message": ")" << status.error_message() << R"("})";
        return {false, 500, oss.str()};
    }
};

// =============================================================================
// JSON Parser
// =============================================================================

class JsonParser {
public:
    JsonParser();
    
    // Request parsing (JSON -> FlatBuffers)
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceFixedRateBondRequestToFBS(const std::string& json);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceFloatingRateBondRequestToFBS(const std::string& json);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceVanillaSwapRequestToFBS(const std::string& json);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceFRARequestToFBS(const std::string& json);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceCapFloorRequestToFBS(const std::string& json);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceSwaptionRequestToFBS(const std::string& json);
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> PriceCDSRequestToFBS(const std::string& json);
    
    // Response generation (FlatBuffers -> JSON)
    std::string FixedRateBondResponseToJSON(const uint8_t* buffer);
    std::string FloatingRateBondResponseToJSON(const uint8_t* buffer);
    std::string VanillaSwapResponseToJSON(const uint8_t* buffer);
    std::string FRAResponseToJSON(const uint8_t* buffer);
    std::string CapFloorResponseToJSON(const uint8_t* buffer);
    std::string SwaptionResponseToJSON(const uint8_t* buffer);
    std::string CDSResponseToJSON(const uint8_t* buffer);

private:
    const char* fbs_include_directories[2] = {FBS_INCLUDE_DIRECTORY, nullptr};
    
    // Request parsers
    std::shared_ptr<flatbuffers::Parser> fixed_rate_bond_request_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> floating_rate_bond_request_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> vanilla_swap_request_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> fra_request_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> cap_floor_request_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> swaption_request_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> cds_request_parser = std::make_shared<flatbuffers::Parser>();
    
    // Response parsers
    std::shared_ptr<flatbuffers::Parser> fixed_rate_bond_response_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> floating_rate_bond_response_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> vanilla_swap_response_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> fra_response_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> cap_floor_response_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> swaption_response_parser = std::make_shared<flatbuffers::Parser>();
    std::shared_ptr<flatbuffers::Parser> cds_response_parser = std::make_shared<flatbuffers::Parser>();
    
    void LoadFBS();
};

// =============================================================================
// Quantra Client
// =============================================================================

class QuantraClient {
public:
    QuantraClient(const std::string& address);
    QuantraClient(const std::string& address, const std::string& schema_dir); // For compatibility
    QuantraClient(const std::string& address, bool use_tls);
    QuantraClient(std::shared_ptr<QuantraServer::Stub> stub);
    
    // JSON API
    JsonResponse PriceFixedRateBondJSON(const std::string& json);
    JsonResponse PriceFloatingRateBondJSON(const std::string& json);
    JsonResponse PriceVanillaSwapJSON(const std::string& json);
    JsonResponse PriceFRAJSON(const std::string& json);
    JsonResponse PriceCapFloorJSON(const std::string& json);
    JsonResponse PriceSwaptionJSON(const std::string& json);
    JsonResponse PriceCDSJSON(const std::string& json);
    
    // Native FlatBuffers API
    template<typename Response>
    using NativeResponse = flatbuffers::grpc::Message<Response>;
    
    grpc::Status PriceFixedRateBond(const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& req, NativeResponse<PriceFixedRateBondResponse>* res);
    grpc::Status PriceFloatingRateBond(const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& req, NativeResponse<PriceFloatingRateBondResponse>* res);
    grpc::Status PriceVanillaSwap(const flatbuffers::grpc::Message<PriceVanillaSwapRequest>& req, NativeResponse<PriceVanillaSwapResponse>* res);
    grpc::Status PriceFRA(const flatbuffers::grpc::Message<PriceFRARequest>& req, NativeResponse<PriceFRAResponse>* res);
    grpc::Status PriceCapFloor(const flatbuffers::grpc::Message<PriceCapFloorRequest>& req, NativeResponse<PriceCapFloorResponse>* res);
    grpc::Status PriceSwaption(const flatbuffers::grpc::Message<PriceSwaptionRequest>& req, NativeResponse<PriceSwaptionResponse>* res);
    grpc::Status PriceCDS(const flatbuffers::grpc::Message<PriceCDSRequest>& req, NativeResponse<PriceCDSResponse>* res);
    
    std::shared_ptr<QuantraServer::Stub> GetStub() { return stub_; }

private:
    std::shared_ptr<QuantraServer::Stub> stub_;
    std::unique_ptr<JsonParser> json_parser_;
    
    void InitChannel(const std::string& address, bool use_tls);
};

// =============================================================================
// Implementation - JsonParser
// =============================================================================

inline JsonParser::JsonParser() {
    this->LoadFBS();
}

inline void JsonParser::LoadFBS() {
    std::string schemafile;
    
    // =========================================================================
    // Fixed Rate Bond
    // =========================================================================
    if (!flatbuffers::LoadFile(FIXED_RATE_BOND_REQUEST_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " FIXED_RATE_BOND_REQUEST_PATH);
    }
    this->fixed_rate_bond_request_parser->opts.strict_json = true;
    if (!this->fixed_rate_bond_request_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing fixed rate bond request schema");
    }
    
    if (!flatbuffers::LoadFile(FIXED_RATE_BOND_RESPONSE_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " FIXED_RATE_BOND_RESPONSE_PATH);
    }
    this->fixed_rate_bond_response_parser->opts.strict_json = true;
    if (!this->fixed_rate_bond_response_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing fixed rate bond response schema");
    }
    
    // =========================================================================
    // Floating Rate Bond
    // =========================================================================
    if (!flatbuffers::LoadFile(FLOATING_RATE_BOND_REQUEST_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " FLOATING_RATE_BOND_REQUEST_PATH);
    }
    this->floating_rate_bond_request_parser->opts.strict_json = true;
    if (!this->floating_rate_bond_request_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing floating rate bond request schema");
    }
    
    if (!flatbuffers::LoadFile(FLOATING_RATE_BOND_RESPONSE_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " FLOATING_RATE_BOND_RESPONSE_PATH);
    }
    this->floating_rate_bond_response_parser->opts.strict_json = true;
    if (!this->floating_rate_bond_response_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing floating rate bond response schema");
    }
    
    // =========================================================================
    // Vanilla Swap
    // =========================================================================
    if (!flatbuffers::LoadFile(VANILLA_SWAP_REQUEST_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " VANILLA_SWAP_REQUEST_PATH);
    }
    this->vanilla_swap_request_parser->opts.strict_json = true;
    if (!this->vanilla_swap_request_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing vanilla swap request schema");
    }
    
    if (!flatbuffers::LoadFile(VANILLA_SWAP_RESPONSE_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " VANILLA_SWAP_RESPONSE_PATH);
    }
    this->vanilla_swap_response_parser->opts.strict_json = true;
    if (!this->vanilla_swap_response_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing vanilla swap response schema");
    }
    
    // =========================================================================
    // FRA
    // =========================================================================
    if (!flatbuffers::LoadFile(FRA_REQUEST_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " FRA_REQUEST_PATH);
    }
    this->fra_request_parser->opts.strict_json = true;
    if (!this->fra_request_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing FRA request schema");
    }
    
    if (!flatbuffers::LoadFile(FRA_RESPONSE_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " FRA_RESPONSE_PATH);
    }
    this->fra_response_parser->opts.strict_json = true;
    if (!this->fra_response_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing FRA response schema");
    }
    
    // =========================================================================
    // Cap/Floor
    // =========================================================================
    if (!flatbuffers::LoadFile(CAP_FLOOR_REQUEST_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " CAP_FLOOR_REQUEST_PATH);
    }
    this->cap_floor_request_parser->opts.strict_json = true;
    if (!this->cap_floor_request_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing cap/floor request schema");
    }
    
    if (!flatbuffers::LoadFile(CAP_FLOOR_RESPONSE_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " CAP_FLOOR_RESPONSE_PATH);
    }
    this->cap_floor_response_parser->opts.strict_json = true;
    if (!this->cap_floor_response_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing cap/floor response schema");
    }
    
    // =========================================================================
    // Swaption
    // =========================================================================
    if (!flatbuffers::LoadFile(SWAPTION_REQUEST_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " SWAPTION_REQUEST_PATH);
    }
    this->swaption_request_parser->opts.strict_json = true;
    if (!this->swaption_request_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing swaption request schema");
    }
    
    if (!flatbuffers::LoadFile(SWAPTION_RESPONSE_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " SWAPTION_RESPONSE_PATH);
    }
    this->swaption_response_parser->opts.strict_json = true;
    if (!this->swaption_response_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing swaption response schema");
    }
    
    // =========================================================================
    // CDS
    // =========================================================================
    if (!flatbuffers::LoadFile(CDS_REQUEST_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " CDS_REQUEST_PATH);
    }
    this->cds_request_parser->opts.strict_json = true;
    if (!this->cds_request_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing CDS request schema");
    }
    
    if (!flatbuffers::LoadFile(CDS_RESPONSE_PATH, false, &schemafile)) {
        throw std::runtime_error("Error loading: " CDS_RESPONSE_PATH);
    }
    this->cds_response_parser->opts.strict_json = true;
    if (!this->cds_response_parser->Parse(schemafile.c_str(), fbs_include_directories)) {
        throw std::runtime_error("Error parsing CDS response schema");
    }
}

// =============================================================================
// Request Parsing (JSON -> FlatBuffers)
// =============================================================================

inline std::shared_ptr<flatbuffers::grpc::MessageBuilder> JsonParser::PriceFixedRateBondRequestToFBS(const std::string& json) {
    if (!this->fixed_rate_bond_request_parser->Parse(json.c_str(), fbs_include_directories)) {
        throw std::runtime_error("JSON parse error: " + this->fixed_rate_bond_request_parser->error_);
    }
    return std::make_shared<flatbuffers::grpc::MessageBuilder>(std::move(this->fixed_rate_bond_request_parser->builder_));
}

inline std::shared_ptr<flatbuffers::grpc::MessageBuilder> JsonParser::PriceFloatingRateBondRequestToFBS(const std::string& json) {
    if (!this->floating_rate_bond_request_parser->Parse(json.c_str(), fbs_include_directories)) {
        throw std::runtime_error("JSON parse error: " + this->floating_rate_bond_request_parser->error_);
    }
    return std::make_shared<flatbuffers::grpc::MessageBuilder>(std::move(this->floating_rate_bond_request_parser->builder_));
}

inline std::shared_ptr<flatbuffers::grpc::MessageBuilder> JsonParser::PriceVanillaSwapRequestToFBS(const std::string& json) {
    if (!this->vanilla_swap_request_parser->Parse(json.c_str(), fbs_include_directories)) {
        throw std::runtime_error("JSON parse error: " + this->vanilla_swap_request_parser->error_);
    }
    return std::make_shared<flatbuffers::grpc::MessageBuilder>(std::move(this->vanilla_swap_request_parser->builder_));
}

inline std::shared_ptr<flatbuffers::grpc::MessageBuilder> JsonParser::PriceFRARequestToFBS(const std::string& json) {
    if (!this->fra_request_parser->Parse(json.c_str(), fbs_include_directories)) {
        throw std::runtime_error("JSON parse error: " + this->fra_request_parser->error_);
    }
    return std::make_shared<flatbuffers::grpc::MessageBuilder>(std::move(this->fra_request_parser->builder_));
}

inline std::shared_ptr<flatbuffers::grpc::MessageBuilder> JsonParser::PriceCapFloorRequestToFBS(const std::string& json) {
    if (!this->cap_floor_request_parser->Parse(json.c_str(), fbs_include_directories)) {
        throw std::runtime_error("JSON parse error: " + this->cap_floor_request_parser->error_);
    }
    return std::make_shared<flatbuffers::grpc::MessageBuilder>(std::move(this->cap_floor_request_parser->builder_));
}

inline std::shared_ptr<flatbuffers::grpc::MessageBuilder> JsonParser::PriceSwaptionRequestToFBS(const std::string& json) {
    if (!this->swaption_request_parser->Parse(json.c_str(), fbs_include_directories)) {
        throw std::runtime_error("JSON parse error: " + this->swaption_request_parser->error_);
    }
    return std::make_shared<flatbuffers::grpc::MessageBuilder>(std::move(this->swaption_request_parser->builder_));
}

inline std::shared_ptr<flatbuffers::grpc::MessageBuilder> JsonParser::PriceCDSRequestToFBS(const std::string& json) {
    if (!this->cds_request_parser->Parse(json.c_str(), fbs_include_directories)) {
        throw std::runtime_error("JSON parse error: " + this->cds_request_parser->error_);
    }
    return std::make_shared<flatbuffers::grpc::MessageBuilder>(std::move(this->cds_request_parser->builder_));
}

// =============================================================================
// Response Generation (FlatBuffers -> JSON) - Updated for FlatBuffers v25
// 
// IMPORTANT: In v25, GenerateText was renamed to GenText and the return type
// changed from bool to const char*:
//   - nullptr = SUCCESS
//   - non-null = ERROR MESSAGE
// =============================================================================

inline std::string JsonParser::FixedRateBondResponseToJSON(const uint8_t* buffer) {
    std::string json;
    const char* error = flatbuffers::GenText((*this->fixed_rate_bond_response_parser), buffer, &json);
    if (error != nullptr) {
        throw std::runtime_error(std::string("Failed to generate JSON for FixedRateBond: ") + error);
    }
    return json;
}

inline std::string JsonParser::FloatingRateBondResponseToJSON(const uint8_t* buffer) {
    std::string json;
    const char* error = flatbuffers::GenText((*this->floating_rate_bond_response_parser), buffer, &json);
    if (error != nullptr) {
        throw std::runtime_error(std::string("Failed to generate JSON for FloatingRateBond: ") + error);
    }
    return json;
}

inline std::string JsonParser::VanillaSwapResponseToJSON(const uint8_t* buffer) {
    std::string json;
    const char* error = flatbuffers::GenText((*this->vanilla_swap_response_parser), buffer, &json);
    if (error != nullptr) {
        throw std::runtime_error(std::string("Failed to generate JSON for VanillaSwap: ") + error);
    }
    return json;
}

inline std::string JsonParser::FRAResponseToJSON(const uint8_t* buffer) {
    std::string json;
    const char* error = flatbuffers::GenText((*this->fra_response_parser), buffer, &json);
    if (error != nullptr) {
        throw std::runtime_error(std::string("Failed to generate JSON for FRA: ") + error);
    }
    return json;
}

inline std::string JsonParser::CapFloorResponseToJSON(const uint8_t* buffer) {
    std::string json;
    const char* error = flatbuffers::GenText((*this->cap_floor_response_parser), buffer, &json);
    if (error != nullptr) {
        throw std::runtime_error(std::string("Failed to generate JSON for CapFloor: ") + error);
    }
    return json;
}

inline std::string JsonParser::SwaptionResponseToJSON(const uint8_t* buffer) {
    std::string json;
    const char* error = flatbuffers::GenText((*this->swaption_response_parser), buffer, &json);
    if (error != nullptr) {
        throw std::runtime_error(std::string("Failed to generate JSON for Swaption: ") + error);
    }
    return json;
}

inline std::string JsonParser::CDSResponseToJSON(const uint8_t* buffer) {
    std::string json;
    const char* error = flatbuffers::GenText((*this->cds_response_parser), buffer, &json);
    if (error != nullptr) {
        throw std::runtime_error(std::string("Failed to generate JSON for CDS: ") + error);
    }
    return json;
}

// =============================================================================
// Implementation - QuantraClient
// =============================================================================

inline QuantraClient::QuantraClient(const std::string& address) {
    InitChannel(address, false);
    json_parser_ = std::make_unique<JsonParser>();
}

inline QuantraClient::QuantraClient(const std::string& address, const std::string& /*schema_dir*/) {
    // schema_dir is ignored - paths are hardcoded
    InitChannel(address, false);
    json_parser_ = std::make_unique<JsonParser>();
}

inline QuantraClient::QuantraClient(const std::string& address, bool use_tls) {
    InitChannel(address, use_tls);
    json_parser_ = std::make_unique<JsonParser>();
}

inline QuantraClient::QuantraClient(std::shared_ptr<QuantraServer::Stub> stub) : stub_(stub) {
    json_parser_ = std::make_unique<JsonParser>();
}

inline void QuantraClient::InitChannel(const std::string& address, bool use_tls) {
    grpc::ChannelArguments args;
    args.SetMaxReceiveMessageSize(100 * 1024 * 1024);
    args.SetMaxSendMessageSize(100 * 1024 * 1024);
    
    std::shared_ptr<grpc::ChannelCredentials> creds;
    if (use_tls) {
        creds = grpc::SslCredentials(grpc::SslCredentialsOptions());
    } else {
        creds = grpc::InsecureChannelCredentials();
    }
    
    stub_ = QuantraServer::NewStub(grpc::CreateCustomChannel(address, creds, args));
}

// =============================================================================
// JSON API
// =============================================================================

inline JsonResponse QuantraClient::PriceFixedRateBondJSON(const std::string& json) {
    try {
        auto builder = json_parser_->PriceFixedRateBondRequestToFBS(json);
        auto request = builder->ReleaseMessage<PriceFixedRateBondRequest>();
        
        grpc::ClientContext context;
        flatbuffers::grpc::Message<PriceFixedRateBondResponse> response;
        
        grpc::Status status = stub_->PriceFixedRateBond(&context, request, &response);
        if (!status.ok()) return JsonResponse::GrpcError(status);
        if (!response.data()) return JsonResponse::ServerError("Empty response");
        
        return JsonResponse::Success(json_parser_->FixedRateBondResponseToJSON(response.data()));
    } catch (const std::exception& e) {
        return JsonResponse::BadRequest(e.what());
    }
}

inline JsonResponse QuantraClient::PriceFloatingRateBondJSON(const std::string& json) {
    try {
        auto builder = json_parser_->PriceFloatingRateBondRequestToFBS(json);
        auto request = builder->ReleaseMessage<PriceFloatingRateBondRequest>();
        
        grpc::ClientContext context;
        flatbuffers::grpc::Message<PriceFloatingRateBondResponse> response;
        
        grpc::Status status = stub_->PriceFloatingRateBond(&context, request, &response);
        if (!status.ok()) return JsonResponse::GrpcError(status);
        if (!response.data()) return JsonResponse::ServerError("Empty response");
        
        return JsonResponse::Success(json_parser_->FloatingRateBondResponseToJSON(response.data()));
    } catch (const std::exception& e) {
        return JsonResponse::BadRequest(e.what());
    }
}

inline JsonResponse QuantraClient::PriceVanillaSwapJSON(const std::string& json) {
    try {
        auto builder = json_parser_->PriceVanillaSwapRequestToFBS(json);
        auto request = builder->ReleaseMessage<PriceVanillaSwapRequest>();
        
        grpc::ClientContext context;
        flatbuffers::grpc::Message<PriceVanillaSwapResponse> response;
        
        grpc::Status status = stub_->PriceVanillaSwap(&context, request, &response);
        if (!status.ok()) return JsonResponse::GrpcError(status);
        if (!response.data()) return JsonResponse::ServerError("Empty response");
        
        return JsonResponse::Success(json_parser_->VanillaSwapResponseToJSON(response.data()));
    } catch (const std::exception& e) {
        return JsonResponse::BadRequest(e.what());
    }
}

inline JsonResponse QuantraClient::PriceFRAJSON(const std::string& json) {
    try {
        auto builder = json_parser_->PriceFRARequestToFBS(json);
        auto request = builder->ReleaseMessage<PriceFRARequest>();
        
        grpc::ClientContext context;
        flatbuffers::grpc::Message<PriceFRAResponse> response;
        
        grpc::Status status = stub_->PriceFRA(&context, request, &response);
        if (!status.ok()) return JsonResponse::GrpcError(status);
        if (!response.data()) return JsonResponse::ServerError("Empty response");
        
        return JsonResponse::Success(json_parser_->FRAResponseToJSON(response.data()));
    } catch (const std::exception& e) {
        return JsonResponse::BadRequest(e.what());
    }
}

inline JsonResponse QuantraClient::PriceCapFloorJSON(const std::string& json) {
    try {
        auto builder = json_parser_->PriceCapFloorRequestToFBS(json);
        auto request = builder->ReleaseMessage<PriceCapFloorRequest>();
        
        grpc::ClientContext context;
        flatbuffers::grpc::Message<PriceCapFloorResponse> response;
        
        grpc::Status status = stub_->PriceCapFloor(&context, request, &response);
        if (!status.ok()) return JsonResponse::GrpcError(status);
        if (!response.data()) return JsonResponse::ServerError("Empty response");
        
        return JsonResponse::Success(json_parser_->CapFloorResponseToJSON(response.data()));
    } catch (const std::exception& e) {
        return JsonResponse::BadRequest(e.what());
    }
}

inline JsonResponse QuantraClient::PriceSwaptionJSON(const std::string& json) {
    try {
        auto builder = json_parser_->PriceSwaptionRequestToFBS(json);
        auto request = builder->ReleaseMessage<PriceSwaptionRequest>();
        
        grpc::ClientContext context;
        flatbuffers::grpc::Message<PriceSwaptionResponse> response;
        
        grpc::Status status = stub_->PriceSwaption(&context, request, &response);
        if (!status.ok()) return JsonResponse::GrpcError(status);
        if (!response.data()) return JsonResponse::ServerError("Empty response");
        
        return JsonResponse::Success(json_parser_->SwaptionResponseToJSON(response.data()));
    } catch (const std::exception& e) {
        return JsonResponse::BadRequest(e.what());
    }
}

inline JsonResponse QuantraClient::PriceCDSJSON(const std::string& json) {
    try {
        auto builder = json_parser_->PriceCDSRequestToFBS(json);
        auto request = builder->ReleaseMessage<PriceCDSRequest>();
        
        grpc::ClientContext context;
        flatbuffers::grpc::Message<PriceCDSResponse> response;
        
        grpc::Status status = stub_->PriceCDS(&context, request, &response);
        if (!status.ok()) return JsonResponse::GrpcError(status);
        if (!response.data()) return JsonResponse::ServerError("Empty response");
        
        return JsonResponse::Success(json_parser_->CDSResponseToJSON(response.data()));
    } catch (const std::exception& e) {
        return JsonResponse::BadRequest(e.what());
    }
}

// =============================================================================
// Native FlatBuffers API
// =============================================================================

inline grpc::Status QuantraClient::PriceFixedRateBond(const flatbuffers::grpc::Message<PriceFixedRateBondRequest>& req, NativeResponse<PriceFixedRateBondResponse>* res) {
    grpc::ClientContext ctx;
    return stub_->PriceFixedRateBond(&ctx, req, res);
}

inline grpc::Status QuantraClient::PriceFloatingRateBond(const flatbuffers::grpc::Message<PriceFloatingRateBondRequest>& req, NativeResponse<PriceFloatingRateBondResponse>* res) {
    grpc::ClientContext ctx;
    return stub_->PriceFloatingRateBond(&ctx, req, res);
}

inline grpc::Status QuantraClient::PriceVanillaSwap(const flatbuffers::grpc::Message<PriceVanillaSwapRequest>& req, NativeResponse<PriceVanillaSwapResponse>* res) {
    grpc::ClientContext ctx;
    return stub_->PriceVanillaSwap(&ctx, req, res);
}

inline grpc::Status QuantraClient::PriceFRA(const flatbuffers::grpc::Message<PriceFRARequest>& req, NativeResponse<PriceFRAResponse>* res) {
    grpc::ClientContext ctx;
    return stub_->PriceFRA(&ctx, req, res);
}

inline grpc::Status QuantraClient::PriceCapFloor(const flatbuffers::grpc::Message<PriceCapFloorRequest>& req, NativeResponse<PriceCapFloorResponse>* res) {
    grpc::ClientContext ctx;
    return stub_->PriceCapFloor(&ctx, req, res);
}

inline grpc::Status QuantraClient::PriceSwaption(const flatbuffers::grpc::Message<PriceSwaptionRequest>& req, NativeResponse<PriceSwaptionResponse>* res) {
    grpc::ClientContext ctx;
    return stub_->PriceSwaption(&ctx, req, res);
}

inline grpc::Status QuantraClient::PriceCDS(const flatbuffers::grpc::Message<PriceCDSRequest>& req, NativeResponse<PriceCDSResponse>* res) {
    grpc::ClientContext ctx;
    return stub_->PriceCDS(&ctx, req, res);
}

} // namespace quantra

#endif // QUANTRA_CLIENT_H