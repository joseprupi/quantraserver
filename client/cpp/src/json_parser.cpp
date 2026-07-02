/**
 * @file json_parser.cpp
 * @brief JSON Parser implementation - handles JSON <-> FlatBuffers conversion
 */

#include "quantra_client.h"

#include <map>
#include <stdexcept>
#include <iostream>
#include <cstdlib>
#include <filesystem>

namespace quantra {

namespace {

std::string ResolveEnvOrDefault(const char* env_name, const std::string& fallback) {
    const char* value = std::getenv(env_name);
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    return fallback;
}

} // namespace

std::string Config::GetFbsDir() {
    return ResolveEnvOrDefault("QUANTRA_FBS_DIR", "flatbuffers/fbs");
}

std::string Config::GetFbsIncludeDir() {
    return ResolveEnvOrDefault("QUANTRA_FBS_INCLUDE_DIR", Config::GetFbsDir());
}

// =============================================================================
// JsonParser Implementation (PIMPL)
// =============================================================================

class JsonParser::Impl {
public:
    Impl() {
        fbs_dir_ = Config::GetFbsDir();
        include_dir_ = Config::GetFbsIncludeDir();
        include_dirs_[0] = include_dir_.c_str();
        include_dirs_[1] = nullptr;
        // Build the calling (main) thread's parser set eagerly so a missing or
        // malformed schema fails fast at startup rather than on the first
        // request handled by a worker thread.
        EnsureThreadParsers();
    }

    std::shared_ptr<flatbuffers::grpc::MessageBuilder> ParseRequest(
        ProductType type,
        const std::string& json
    ) {
        // The body is handed to the flatc parser as a C string below, so an
        // embedded NUL would silently truncate the request. Reject
        // it explicitly instead.
        if (json.find('\0') != std::string::npos) {
            throw JsonParseException(
                "request body contains a NUL byte (bodies must be NUL-free JSON text)");
        }

        auto& parser = EnsureThreadParsers().request_parsers.at(type);

        // TODO: flatc's parser error text for malformed JSON can be
        // misleading ("unknown field ...", spurious "required" complaints,
        // "input file is empty" for wrong-shape bodies). The wording comes from
        // the vendored FlatBuffers parser internals — out of scope to fix here.
        if (!parser->Parse(json.c_str(), include_dirs_)) {
            throw JsonParseException("JSON parse error for " +
                std::string(ProductTypeToString(type)) + ": " + parser->error_);
        }

        return std::make_shared<flatbuffers::grpc::MessageBuilder>(
            std::move(parser->builder_)
        );
    }

    std::string GenerateResponse(ProductType type, const uint8_t* buffer) {
        auto& parser = EnsureThreadParsers().response_parsers.at(type);

        std::string json;
        // FlatBuffers v24+: GenText returns const char* (nullptr = success)
        const char* error = flatbuffers::GenText(*parser, buffer, &json);

        if (error != nullptr) {
            throw JsonRuntimeException("Failed to generate JSON for " +
                std::string(ProductTypeToString(type)) + ": " + error);
        }

        return json;
    }

private:
    struct ThreadParsers {
        std::map<ProductType, std::shared_ptr<flatbuffers::Parser>> request_parsers;
        std::map<ProductType, std::shared_ptr<flatbuffers::Parser>> response_parsers;
    };

    const char* include_dirs_[2];
    std::string fbs_dir_;
    std::string include_dir_;

    // Returns the current thread's parser set, loading the schemas on first use.
    ThreadParsers& EnsureThreadParsers() {
        thread_local ThreadParsers parsers;
        thread_local bool loaded = false;
        if (!loaded) {
            const auto& schemas = GetProductSchemas();
            for (const auto& [type, schema] : schemas) {
                parsers.request_parsers[type] = LoadSchema(schema.request_file);
                parsers.response_parsers[type] = LoadSchema(schema.response_file);
            }
            loaded = true;
        }
        return parsers;
    }

    std::shared_ptr<flatbuffers::Parser> LoadSchema(const char* filename) {
        auto parser = std::make_shared<flatbuffers::Parser>();
        parser->opts.strict_json = true;
        parser->opts.force_defaults = true;
        
        std::filesystem::path filepath = std::filesystem::path(fbs_dir_) / filename;
        std::string content;
        
        if (!flatbuffers::LoadFile(filepath.string().c_str(), false, &content)) {
            throw JsonRuntimeException(
                "Failed to load schema: " + filepath.string() +
                " (set QUANTRA_FBS_DIR/QUANTRA_FBS_INCLUDE_DIR if needed)");
        }
        
        if (!parser->Parse(content.c_str(), include_dirs_)) {
            throw JsonRuntimeException("Failed to parse schema " +
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