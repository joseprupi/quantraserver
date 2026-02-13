/**
 * @file json_parser.cpp
 * @brief JSON Parser implementation - handles JSON <-> FlatBuffers conversion
 */

#include "quantra_client.h"
#include "product_registry.h"

#include <map>
#include <stdexcept>
#include <iostream>

namespace quantra {

// =============================================================================
// JsonParser Implementation (PIMPL)
// =============================================================================

class JsonParser::Impl {
public:
    Impl() {
        include_dirs_[0] = Config::FBS_INCLUDE_DIR;
        include_dirs_[1] = nullptr;
        LoadAllSchemas();
    }
    
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> ParseRequest(
        ProductType type, 
        const std::string& json
    ) {
        auto& parser = request_parsers_.at(type);
        
        if (!parser->Parse(json.c_str(), include_dirs_)) {
            throw std::runtime_error("JSON parse error for " + 
                std::string(ProductTypeToString(type)) + ": " + parser->error_);
        }
        
        return std::make_shared<flatbuffers::grpc::MessageBuilder>(
            std::move(parser->builder_)
        );
    }
    
    std::string GenerateResponse(ProductType type, const uint8_t* buffer) {
        auto& parser = response_parsers_.at(type);
        
        std::string json;
        // FlatBuffers v24+: GenText returns const char* (nullptr = success)
        const char* error = flatbuffers::GenText(*parser, buffer, &json);
        
        if (error != nullptr) {
            throw std::runtime_error("Failed to generate JSON for " + 
                std::string(ProductTypeToString(type)) + ": " + error);
        }
        
        return json;
    }

private:
    const char* include_dirs_[2];
    std::map<ProductType, std::shared_ptr<flatbuffers::Parser>> request_parsers_;
    std::map<ProductType, std::shared_ptr<flatbuffers::Parser>> response_parsers_;
    
    void LoadAllSchemas() {
        const auto& schemas = GetProductSchemas();
        
        for (const auto& [type, schema] : schemas) {
            request_parsers_[type] = LoadSchema(schema.request_file);
            response_parsers_[type] = LoadSchema(schema.response_file);
        }
    }
    
    std::shared_ptr<flatbuffers::Parser> LoadSchema(const char* filename) {
        auto parser = std::make_shared<flatbuffers::Parser>();
        parser->opts.strict_json = true;
        parser->opts.force_defaults = true;
        
        std::string filepath = std::string(Config::FBS_DIR) + "/" + filename;
        std::string content;
        
        if (!flatbuffers::LoadFile(filepath.c_str(), false, &content)) {
            throw std::runtime_error("Failed to load schema: " + filepath);
        }
        
        if (!parser->Parse(content.c_str(), include_dirs_)) {
            throw std::runtime_error("Failed to parse schema " + 
                std::string(filename) + ": " + parser->error_);
        }
        
        return parser;
    }
};

// =============================================================================
// JsonParser Public Interface
// =============================================================================

JsonParser::JsonParser() : impl_(std::make_unique<Impl>()) {}

JsonParser::~JsonParser() = default;

std::shared_ptr<flatbuffers::grpc::MessageBuilder> JsonParser::ParseRequest(
    ProductType type, 
    const std::string& json
) {
    return impl_->ParseRequest(type, json);
}

std::string JsonParser::GenerateResponse(ProductType type, const uint8_t* buffer) {
    return impl_->GenerateResponse(type, buffer);
}

} // namespace quantra