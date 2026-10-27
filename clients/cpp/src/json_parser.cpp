/**
 * @file json_parser.cpp
 * @brief JSON Parser implementation - handles JSON <-> FlatBuffers conversion
 */

#include "non_finite_scan.h"
#include "quantra_client.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace quantra {

namespace {

std::string ResolveEnvOrDefault(const char* env_name, const std::string& fallback) {
    const char* value = std::getenv(env_name);
    if (value != nullptr && value[0] != '\0') {
        return value;
    }
    return fallback;
}

std::string LineColumn(const std::string& text, std::size_t pos) {
    std::size_t line = 1;
    std::size_t column = 1;
    for (std::size_t i = 0; i < pos && i < text.size(); ++i) {
        if (text[i] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return "line " + std::to_string(line) + ", column " + std::to_string(column);
}

// A request body must be one JSON object. The flatc parser is more lenient:
// it reads schema statements (include, namespace, table, root_type,
// attribute, ...) before the object and skips comments anywhere. Returns why
// the body breaks the rule, or std::nullopt. With '{' as the first token,
// Parse() reads nothing but the object; text after it is rejected by the
// parser itself.
std::optional<std::string> CheckSingleJsonObject(const std::string& text) {
    const std::size_t n = text.size();
    const auto comment_at = [&](std::size_t pos) {
        return text[pos] == '/' && pos + 1 < n && (text[pos + 1] == '/' || text[pos + 1] == '*');
    };
    const auto comment_error = [&](std::size_t pos) {
        return "comment at " + LineColumn(text, pos) + " (comments are not JSON)";
    };

    std::size_t i = 0;
    // A leading UTF-8 byte order mark is skipped, as the parser does.
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        i = 3;
    }
    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r')) {
        ++i;
    }
    if (i < n && comment_at(i)) {
        return comment_error(i);
    }
    if (i == n || text[i] != '{') {
        return "expected '{' at " + LineColumn(text, i);
    }
    // Strings are delimited as the parser's tokenizer does: by the quote that
    // opened them (" or '), with backslash escapes.
    char quote = 0;
    for (; i < n; ++i) {
        const char c = text[i];
        if (quote != 0) {
            if (c == '\\') {
                ++i;
            } else if (c == quote) {
                quote = 0;
            }
        } else if (c == '"' || c == '\'') {
            quote = c;
        } else if (comment_at(i)) {
            return comment_error(i);
        }
    }
    return std::nullopt;
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
        ProductType type, const std::string& json) {
        // The body is handed to the flatc parser as a C string below, so an
        // embedded NUL would silently truncate the request. Reject
        // it explicitly instead.
        if (json.find('\0') != std::string::npos) {
            throw JsonParseException(
                "request body contains a NUL byte (bodies must be NUL-free JSON text)");
        }
        // Schema statements would otherwise be applied to this thread's
        // long-lived parser and change how later bodies are decoded.
        if (auto problem = CheckSingleJsonObject(json)) {
            throw JsonParseException("request body must be a single JSON object: " + *problem);
        }

        auto& parser = EnsureThreadParsers().request_parsers.at(type);

        // TODO: flatc's parser error text for malformed JSON can be
        // misleading ("unknown field ...", spurious "required" complaints,
        // "input file is empty" for wrong-shape bodies). The wording comes from
        // the vendored FlatBuffers parser internals — out of scope to fix here.
        //
        // Parse() rather than ParseJson(): ParseJson() skips the reset of the
        // parser's per-parse state, so every failed body would leave its
        // partly parsed values behind in this long-lived parser.
        if (!parser->Parse(json.c_str(), include_dirs_)) {
            throw JsonParseException(
                "JSON parse error for " + std::string(ProductTypeToString(type)) + ": " +
                parser->error_);
        }
        // A parse that read no object leaves no finished buffer; releasing it
        // would abort.
        if (parser->builder_.GetSize() == 0) {
            throw JsonParseException(
                "JSON parse error for " + std::string(ProductTypeToString(type)) +
                ": no JSON object in the request body");
        }

        return std::make_shared<flatbuffers::grpc::MessageBuilder>(std::move(parser->builder_));
    }

    std::string GenerateResponse(ProductType type, const uint8_t* buffer) {
        auto& parser = EnsureThreadParsers().response_parsers.at(type);

        std::string json;
        // FlatBuffers v24+: GenText returns const char* (nullptr = success)
        const char* error = flatbuffers::GenText(*parser, buffer, &json);

        if (error != nullptr) {
            throw JsonRuntimeException(
                "Failed to generate JSON for " + std::string(ProductTypeToString(type)) + ": " +
                error);
        }

        // FlatBuffers prints a non-finite double as a bare `inf` / `-inf` /
        // `nan`, which strict JSON parsers reject. Refuse to ship such a body:
        // report the field as an internal error (HTTP 500 via
        // JsonResponse::ServerError) and log it, instead of returning a 200
        // whose body is not JSON.
        if (auto path = FindNonFiniteValue(json)) {
            std::cerr << "[QuantraClient] non-finite value in response"
                      << " product=" << ProductTypeToString(type) << " path=" << *path << std::endl;
            throw JsonRuntimeException("non-finite value in response at " + *path);
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
            throw JsonRuntimeException(
                "Failed to parse schema " + std::string(filename) + ": " + parser->error_);
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
    ProductType type, const std::string& json) {
    return impl_->ParseRequest(type, json);
}

std::string JsonParser::GenerateResponse(ProductType type, const uint8_t* buffer) {
    return impl_->GenerateResponse(type, buffer);
}

} // namespace quantra