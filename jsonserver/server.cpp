/**
 * @file server.cpp
 * @brief Quantra JSON HTTP API Server
 * 
 * Thin HTTP layer using Crow that routes to the QuantraClient library.
 * 
 * Usage:
 *   ./json_server <grpc_address> <http_port>
 *   ./json_server localhost:50051 8080
 */

#include <iostream>
#include <string>
#include <cstdlib>
#include <exception>
#include <vector>
#include <chrono>
#include <cstdint>
#include <optional>
#include <utility>
#include <algorithm>
#include <sstream>
#include <cctype>
#include <boost/asio/ip/tcp.hpp>

// Tell Crow to use Boost.Asio instead of standalone Asio
#define CROW_USE_BOOST 1

#include "crow_all.h"
#include "quantra_client.h"
#include "product_catalog.h"

#ifndef QUANTRA_GIT_SHA
#define QUANTRA_GIT_SHA "unknown"
#endif
#ifndef QUANTRA_BUILD_TIME_UTC
#define QUANTRA_BUILD_TIME_UTC "unknown"
#endif
#ifndef QUANTRA_API_VERSION
#define QUANTRA_API_VERSION "unknown"
#endif
#ifndef QUANTRA_BACKEND_VERSION
#define QUANTRA_BACKEND_VERSION "unknown"
#endif
#ifndef QUANTRA_OPENAPI_VERSION
#define QUANTRA_OPENAPI_VERSION "unknown"
#endif
#ifndef QUANTRA_GRPC_VERSION
#define QUANTRA_GRPC_VERSION "unknown"
#endif
#ifndef QUANTRA_FLATBUFFERS_VERSION
#define QUANTRA_FLATBUFFERS_VERSION "unknown"
#endif
#ifndef QUANTRA_QUANTLIB_VERSION
#define QUANTRA_QUANTLIB_VERSION "unknown"
#endif

using namespace quantra;

namespace {

std::string TrimWhitespace(const std::string& value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c) != 0;
    }).base();
    if (begin >= end) {
        return "";
    }
    return std::string(begin, end);
}

bool HasWhitespace(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c) != 0;
    });
}

std::optional<int> ParsePortNumber(const std::string& text, std::string& error_out) {
    const std::string trimmed = TrimWhitespace(text);
    if (trimmed.empty()) {
        error_out = "must not be empty";
        return std::nullopt;
    }

    size_t parsed_chars = 0;
    int64_t parsed = 0;
    try {
        parsed = std::stoll(trimmed, &parsed_chars, 10);
    } catch (const std::exception&) {
        error_out = "must be numeric";
        return std::nullopt;
    }

    if (parsed_chars != trimmed.size()) {
        error_out = "must be numeric";
        return std::nullopt;
    }
    if (parsed < 1 || parsed > 65535) {
        error_out = "must be between 1 and 65535";
        return std::nullopt;
    }

    return static_cast<int>(parsed);
}

std::string GrpcChannelStateToString(grpc_connectivity_state state) {
    switch (state) {
        case GRPC_CHANNEL_IDLE: return "IDLE";
        case GRPC_CHANNEL_CONNECTING: return "CONNECTING";
        case GRPC_CHANNEL_READY: return "READY";
        case GRPC_CHANNEL_TRANSIENT_FAILURE: return "TRANSIENT_FAILURE";
        case GRPC_CHANNEL_SHUTDOWN: return "SHUTDOWN";
        default: return "UNKNOWN";
    }
}

std::optional<std::pair<std::string, std::string>> ParseHostPort(const std::string& in) {
    const std::string trimmed = TrimWhitespace(in);
    if (trimmed.empty()) {
        return std::nullopt;
    }

    if (trimmed.front() == '[') {
        const auto close = trimmed.find(']');
        if (close == std::string::npos || close + 1 >= trimmed.size() || trimmed[close + 1] != ':') {
            return std::nullopt;
        }

        const std::string host = trimmed.substr(0, close + 1);
        const std::string port = trimmed.substr(close + 2);
        if (port.empty() || HasWhitespace(host) || HasWhitespace(port)) {
            return std::nullopt;
        }

        return std::make_pair(host, port);
    }

    const auto pos = trimmed.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= trimmed.size()) {
        return std::nullopt;
    }
    if (trimmed.find(':') != pos) {
        return std::nullopt;
    }

    const std::string host = trimmed.substr(0, pos);
    const std::string port = trimmed.substr(pos + 1);
    if (host.empty() || port.empty() || HasWhitespace(host) || HasWhitespace(port)) {
        return std::nullopt;
    }

    return std::make_pair(host, port);
}

// QUANTRA_ENVOY_ADMIN is naturally written as a URL (the Dockerfile defaults it
// to http://127.0.0.1:9901), so accept both host:port and a scheme-prefixed URL:
// strip a leading http:// or https:// and anything from the first '/' on before
// host:port parsing.
std::optional<std::pair<std::string, std::string>> ParseEnvoyAdminTarget(const std::string& in) {
    std::string target = TrimWhitespace(in);
    for (const std::string scheme : {"http://", "https://"}) {
        if (target.size() > scheme.size() &&
            std::equal(scheme.begin(), scheme.end(), target.begin(), [](char a, char b) {
                return a == static_cast<char>(std::tolower(static_cast<unsigned char>(b)));
            })) {
            target.erase(0, scheme.size());
            break;
        }
    }
    const auto slash = target.find('/');
    if (slash != std::string::npos) {
        target.erase(slash);
    }
    return ParseHostPort(target);
}

bool ValidateGrpcAddress(const std::string& grpc_address, std::string& error_out) {
    const auto host_port = ParseHostPort(grpc_address);
    if (!host_port) {
        error_out = "must be in host:port or [ipv6]:port form";
        return false;
    }

    std::string port_error;
    if (!ParsePortNumber(host_port->second, port_error)) {
        error_out = "port " + port_error;
        return false;
    }

    return true;
}

bool IsJsonContentType(const crow::request& req) {
    std::string ct = req.get_header_value("Content-Type");
    std::transform(ct.begin(), ct.end(), ct.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ct.find("application/json") != std::string::npos;
}

constexpr size_t kMaxJsonRequestBytes = 10 * 1024 * 1024;

std::optional<crow::response> RejectInvalidJsonRequest(const crow::request& req) {
    if (!IsJsonContentType(req)) {
        return crow::response(415, R"({"error":"Content-Type must be application/json"})");
    }
    if (req.body.size() > kMaxJsonRequestBytes) {
        return crow::response(413, R"({"error":"Request body too large"})");
    }
    // Reject an empty/whitespace-only body up front: forwarded to the flatc
    // parser it yields the misleading "input file is empty".
    if (TrimWhitespace(req.body).empty()) {
        return crow::response(400, R"({"error":"empty request body"})");
    }
    return std::nullopt;
}

// Sanitize a caller-supplied X-Request-Id before it is used as gRPC metadata,
// echoed as a response header, or written to logs: keep only
// printable non-space ASCII (drops CR/LF and other control characters) and cap
// the length. Returns "" when nothing usable remains.
constexpr size_t kMaxRequestIdLen = 128;

std::string SanitizeRequestId(const std::string& raw) {
    std::string out;
    out.reserve(std::min(raw.size(), kMaxRequestIdLen));
    for (char c : raw) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (uc <= 0x20 || uc >= 0x7f) {
            continue;
        }
        out += c;
        if (out.size() >= kMaxRequestIdLen) {
            break;
        }
    }
    return out;
}

std::optional<std::string> HttpGetWithTimeout(
    const std::string& host,
    const std::string& port,
    const std::string& target,
    std::chrono::milliseconds timeout,
    std::string& error_out) {
    boost::asio::ip::tcp::iostream stream;
    stream.expires_after(timeout);
    stream.connect(host, port);
    if (!stream) {
        error_out = "connect failed";
        return std::nullopt;
    }

    stream << "GET " << target << " HTTP/1.1\r\n"
           << "Host: " << host << "\r\n"
           << "User-Agent: quantra-json-status/1.0\r\n"
           << "Accept: application/json\r\n"
           << "Connection: close\r\n\r\n";
    stream.flush();

    std::string response((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
    auto hdr_end = response.find("\r\n\r\n");
    if (hdr_end == std::string::npos) {
        error_out = "invalid HTTP response";
        return std::nullopt;
    }
    std::string headers = response.substr(0, hdr_end);
    std::string body = response.substr(hdr_end + 4);

    std::string headers_lc = headers;
    std::transform(headers_lc.begin(), headers_lc.end(), headers_lc.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (headers_lc.find("transfer-encoding: chunked") != std::string::npos) {
        std::string decoded;
        size_t pos = 0;
        while (true) {
            size_t line_end = body.find("\r\n", pos);
            if (line_end == std::string::npos) {
                error_out = "invalid chunked response";
                return std::nullopt;
            }
            std::string size_hex = body.substr(pos, line_end - pos);
            size_t semi = size_hex.find(';');
            if (semi != std::string::npos) size_hex = size_hex.substr(0, semi);
            size_t chunk_size = 0;
            try {
                chunk_size = std::stoul(size_hex, nullptr, 16);
            } catch (...) {
                error_out = "invalid chunk size";
                return std::nullopt;
            }
            pos = line_end + 2;
            if (chunk_size == 0) break;
            if (pos > body.size() || chunk_size > body.size() - pos) {
                error_out = "truncated chunked body";
                return std::nullopt;
            }
            decoded.append(body.substr(pos, chunk_size));
            pos += chunk_size;
            if (pos > body.size() || body.size() - pos < 2 || body.substr(pos, 2) != "\r\n") {
                error_out = "invalid chunk delimiter";
                return std::nullopt;
            }
            pos += 2;
        }
        return decoded;
    }

    return body;
}

void FillEnvoyClusterHealth(
    crow::json::wvalue& status_out,
    const std::string& envoy_admin_target) {
    crow::json::wvalue envoy;
    envoy["configured"] = true;
    envoy["admin_target"] = envoy_admin_target;

    auto hp = ParseEnvoyAdminTarget(envoy_admin_target);
    if (!hp) {
        envoy["reachable"] = false;
        envoy["error"] = "invalid QUANTRA_ENVOY_ADMIN target, expected host:port or http://host:port";
        status_out["envoy"] = std::move(envoy);
        return;
    }

    std::string fetch_err;
    auto body = HttpGetWithTimeout(
        hp->first,
        hp->second,
        "/stats?filter=cluster.quantra_workers.membership_",
        std::chrono::milliseconds(250),
        fetch_err);
    if (!body) {
        envoy["reachable"] = false;
        envoy["error"] = fetch_err;
        status_out["envoy"] = std::move(envoy);
        return;
    }
    envoy["reachable"] = true;

    std::string json_text = *body;
    auto first_brace = json_text.find('{');
    auto last_brace = json_text.rfind('}');
    if (first_brace != std::string::npos && last_brace != std::string::npos && last_brace >= first_brace) {
        json_text = json_text.substr(first_brace, last_brace - first_brace + 1);
    }

    int total = -1;
    int healthy = -1;
    int degraded = 0;
    std::istringstream ss(json_text);
    std::string line;
    while (std::getline(ss, line)) {
        auto parse_metric = [&](const std::string& key, int& out) {
            auto p = line.find(key);
            if (p == std::string::npos) return;
            auto c = line.find(':', p);
            if (c == std::string::npos) return;
            try {
                out = std::stoi(line.substr(c + 1));
            } catch (...) {
                std::cerr << "[jsonserver] failed to parse envoy metric "
                          << key << " from line: " << line << std::endl;
            }
        };
        parse_metric("cluster.quantra_workers.membership_total", total);
        parse_metric("cluster.quantra_workers.membership_healthy", healthy);
        parse_metric("cluster.quantra_workers.membership_degraded", degraded);
    }

    if (total < 0 || healthy < 0) {
        envoy["error"] = "failed to parse envoy worker membership stats";
        status_out["envoy"] = std::move(envoy);
        return;
    }

    envoy["cluster"] = "quantra_workers";
    envoy["healthy_workers"] = healthy;
    envoy["total_workers"] = total;
    envoy["unhealthy_workers"] = total - healthy;
    envoy["degraded_workers"] = degraded;
    status_out["envoy"] = std::move(envoy);
}

} // namespace

void PrintUsage(const char* program) {
    std::cerr << "Quantra JSON API Server\n\n"
              << "Usage: " << program << " <grpc_server> <http_port> [--log-json]\n\n"
              << "Arguments:\n"
              << "  grpc_server  - Quantra gRPC server address (e.g., localhost:50051)\n"
              << "  http_port    - HTTP port to listen on (e.g., 8080)\n"
              << "  --log-json   - Log full JSON request bodies for POST endpoints\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        PrintUsage(argv[0]);
        return 1;
    }
    
    std::string grpc_address = argv[1];
    std::string http_port_error;
    auto http_port = ParsePortNumber(argv[2], http_port_error);
    if (!http_port) {
        std::cerr << "Invalid HTTP port '" << argv[2] << "': " << http_port_error << "\n";
        return 1;
    }

    std::string grpc_address_error;
    if (!ValidateGrpcAddress(grpc_address, grpc_address_error)) {
        std::cerr << "Invalid gRPC server address '" << grpc_address << "': "
                  << grpc_address_error << "\n";
        return 1;
    }

    bool log_json_bodies = false;
    for (int i = 3; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--log-json") {
            log_json_bodies = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            PrintUsage(argv[0]);
            return 1;
        }
    }
    auto start_time = std::chrono::steady_clock::now();
    const char* envoy_admin_env = std::getenv("QUANTRA_ENVOY_ADMIN");
    std::string envoy_admin_target = envoy_admin_env ? envoy_admin_env : "";

    std::vector<std::string> product_names;
    for (const auto& kv : GetProductCatalog()) {
        product_names.emplace_back(ProductTypeToString(kv.first));
    }
    std::vector<std::string> endpoint_list = GetProductEndpointList();
    endpoint_list.push_back("GET /status");
    endpoint_list.push_back("GET /meta");
    endpoint_list.push_back("GET /health");
    
    std::cout << "===========================================\n"
              << "  Quantra JSON API Server\n"
              << "===========================================\n"
              << "  gRPC backend: " << grpc_address << "\n"
              << "  HTTP port:    " << *http_port << "\n"
              << "  Log JSON:     " << (log_json_bodies ? "enabled" : "disabled") << "\n"
              << "===========================================\n\n";
    
    try {
        // Initialize client
        QuantraClient client(grpc_address);
        
        // HTTP server
        crow::SimpleApp app;
        auto log_json_request = [&](const char* route, const crow::request& req) {
            if (!log_json_bodies) {
                return;
            }
            std::cout << "[jsonserver] POST " << route
                      << " body_bytes=" << req.body.size() << "\n"
                      << "[jsonserver] JSON BEGIN " << route << "\n"
                      << req.body << "\n"
                      << "[jsonserver] JSON END " << route << std::endl;
        };
        // Shared POST wrapper. Every response carries X-Quantra-Api-Version so
        // callers can detect the server's API version on any call. The
        // caller's X-Request-Id (sanitized) is echoed back as a response
        // header and forwarded to the gRPC backend by fn so engine logs
        // correlate with the HTTP caller.
        auto respond = [&](const char* route, const crow::request& req, auto&& fn) {
            const std::string request_id =
                SanitizeRequestId(req.get_header_value("X-Request-Id"));
            auto with_common_headers = [&](crow::response resp) {
                resp.set_header("X-Quantra-Api-Version", QUANTRA_API_VERSION);
                if (!request_id.empty()) {
                    resp.set_header("X-Request-Id", request_id);
                }
                return resp;
            };
            if (auto rejected = RejectInvalidJsonRequest(req)) {
                return with_common_headers(std::move(*rejected));
            }
            log_json_request(route, req);
            auto r = fn(req.body, request_id);
            if (r.status_code >= 400) {
                std::cerr << "[jsonserver] backend_error " << route
                          << " http_status=" << r.status_code
                          << (request_id.empty() ? std::string()
                                                 : " request_id=" + request_id)
                          << " response=" << r.body << std::endl;
            }
            return with_common_headers(crow::response(r.status_code, r.body));
        };
        
        // Health checks
        CROW_ROUTE(app, "/")
        ([]() { return R"({"status":"ok","service":"quantra-json-api"})"; });
        
        CROW_ROUTE(app, "/health")
        ([]() { return R"({"status":"healthy"})"; });

        CROW_ROUTE(app, "/status")
        ([&]() {
            crow::json::wvalue out;
            out["service"] = "quantra-json-api";
            out["status"] = "ok";
            out["grpc_target"] = client.GetGrpcTarget();
            bool grpc_ready = client.WaitForChannelReady(std::chrono::milliseconds(75));
            auto state = client.GetChannelState(false);
            out["grpc_channel_state"] = GrpcChannelStateToString(state);
            out["grpc_ready"] = grpc_ready;
            out["uptime_seconds"] = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::steady_clock::now() - start_time).count());

            if (!envoy_admin_target.empty()) {
                FillEnvoyClusterHealth(out, envoy_admin_target);
            } else {
                crow::json::wvalue envoy;
                envoy["configured"] = false;
                out["envoy"] = std::move(envoy);
            }
            return crow::response(200, out.dump());
        });

        CROW_ROUTE(app, "/meta")
        ([&]() {
            crow::json::wvalue out;
            out["service"] = "quantra-json-api";
            out["status"] = "healthy";
            out["api_version"] = QUANTRA_API_VERSION;
            out["backend_version"] = QUANTRA_BACKEND_VERSION;
            out["git_sha"] = QUANTRA_GIT_SHA;
            out["build_time_utc"] = QUANTRA_BUILD_TIME_UTC;
            out["openapi_version"] = QUANTRA_OPENAPI_VERSION;
            out["grpc_target"] = grpc_address;

            crow::json::wvalue::list products;
            for (const auto& p : product_names) {
                products.emplace_back(p);
            }
            out["products"] = std::move(products);

            crow::json::wvalue::list endpoints;
            for (const auto& ep : endpoint_list) {
                endpoints.emplace_back(ep);
            }
            out["endpoints"] = std::move(endpoints);

            crow::json::wvalue dependencies;
            dependencies["grpc"] = QUANTRA_GRPC_VERSION;
            dependencies["flatbuffers"] = QUANTRA_FLATBUFFERS_VERSION;
            dependencies["quantlib"] = QUANTRA_QUANTLIB_VERSION;
            out["dependencies"] = std::move(dependencies);

            return crow::response(200, out.dump());
        });
        
        // Pricing endpoints
        CROW_ROUTE(app, "/price-fixed-rate-bond").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-fixed-rate-bond", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceFixedRateBondJSON(body, request_id);
            });
        });
        
        CROW_ROUTE(app, "/price-floating-rate-bond").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-floating-rate-bond", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceFloatingRateBondJSON(body, request_id);
            });
        });
        
        CROW_ROUTE(app, "/price-vanilla-swap").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-vanilla-swap", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceVanillaSwapJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/price-zero-coupon-inflation-swap").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-zero-coupon-inflation-swap", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceZeroCouponInflationSwapJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/price-year-on-year-inflation-swap").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-year-on-year-inflation-swap", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceYearOnYearInflationSwapJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/price-ois-swap").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-ois-swap", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceOisSwapJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/price-basis-swap").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-basis-swap", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceBasisSwapJSON(body, request_id);
            });
        });
        
        CROW_ROUTE(app, "/price-fra").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-fra", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceFRAJSON(body, request_id);
            });
        });
        
        CROW_ROUTE(app, "/price-cap-floor").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-cap-floor", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceCapFloorJSON(body, request_id);
            });
        });
        
        CROW_ROUTE(app, "/price-swaption").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-swaption", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceSwaptionJSON(body, request_id);
            });
        });
        
        CROW_ROUTE(app, "/price-cds").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-cds", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceCDSJSON(body, request_id);
            });
        });
        
        CROW_ROUTE(app, "/bootstrap-curves").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/bootstrap-curves", req, [&](const std::string& body, const std::string& request_id) {
                return client.BootstrapCurvesJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/bootstrap-inflation-curves").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/bootstrap-inflation-curves", req, [&](const std::string& body, const std::string& request_id) {
                return client.BootstrapInflationCurvesJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/sample-vol-surfaces").methods("POST"_method)
        ([&](const crow::request& req) {
            const std::string request_id =
                SanitizeRequestId(req.get_header_value("X-Request-Id"));
            auto with_common_headers = [&](crow::response resp) {
                resp.set_header("X-Quantra-Api-Version", QUANTRA_API_VERSION);
                if (!request_id.empty()) {
                    resp.set_header("X-Request-Id", request_id);
                }
                return resp;
            };
            if (auto rejected = RejectInvalidJsonRequest(req)) {
                return with_common_headers(std::move(*rejected));
            }
            log_json_request("/sample-vol-surfaces", req);
            auto r = client.SampleVolSurfacesJSON(req.body, request_id);
            if (r.status_code >= 400) {
                std::cerr << "[jsonserver] backend_error /sample-vol-surfaces"
                          << " http_status=" << r.status_code
                          << (request_id.empty() ? std::string()
                                                 : " request_id=" + request_id)
                          << " response=" << r.body << std::endl;
            } else {
                std::cout << "[jsonserver] /sample-vol-surfaces success"
                          << " http_status=" << r.status_code << std::endl;
            }
            return with_common_headers(crow::response(r.status_code, r.body));
        });

        CROW_ROUTE(app, "/calendar-business-days").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/calendar-business-days", req, [&](const std::string& body, const std::string& request_id) {
                return client.CalendarBusinessDaysJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/calendar-holidays").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/calendar-holidays", req, [&](const std::string& body, const std::string& request_id) {
                return client.CalendarHolidaysJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/calendar-advance").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/calendar-advance", req, [&](const std::string& body, const std::string& request_id) {
                return client.CalendarAdvanceJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/calibrate-swaption-model").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/calibrate-swaption-model", req, [&](const std::string& body, const std::string& request_id) {
                return client.CalibrateSwaptionModelJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/calibrate-swaption-vol").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/calibrate-swaption-vol", req, [&](const std::string& body, const std::string& request_id) {
                return client.CalibrateSwaptionVolJSON(body, request_id);
            });
        });

        CROW_ROUTE(app, "/price-equity-option").methods("POST"_method)
        ([&](const crow::request& req) {
            return respond("/price-equity-option", req, [&](const std::string& body, const std::string& request_id) {
                return client.PriceEquityOptionJSON(body, request_id);
            });
        });
        
        // Print endpoints
        std::cout << "Endpoints:\n"
                  << "  POST /price-fixed-rate-bond\n"
                  << "  POST /price-floating-rate-bond\n"
                  << "  POST /price-vanilla-swap\n"
                  << "  POST /price-zero-coupon-inflation-swap\n"
                  << "  POST /price-year-on-year-inflation-swap\n"
                  << "  POST /price-ois-swap\n"
                  << "  POST /price-basis-swap\n"
                  << "  POST /price-fra\n"
                  << "  POST /price-cap-floor\n"
                  << "  POST /price-swaption\n"
                  << "  POST /price-cds\n"
                  << "  POST /bootstrap-curves\n"
                  << "  POST /bootstrap-inflation-curves\n"
                  << "  POST /sample-vol-surfaces\n"
                  << "  POST /calendar-business-days\n"
                  << "  POST /calendar-holidays\n"
                  << "  POST /calendar-advance\n"
                  << "  POST /calibrate-swaption-model\n"
                  << "  POST /calibrate-swaption-vol\n"
                  << "  POST /price-equity-option\n"
                  << "  GET  /status\n"
                  << "  GET  /meta\n"
                  << "  GET  /health\n\n"
                  << "Starting server...\n";
        
        app.port(*http_port).multithreaded().run();
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}