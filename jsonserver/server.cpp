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

// Tell Crow to use Boost.Asio instead of standalone Asio
#define CROW_USE_BOOST 1

#include "crow_all.h"
#include "quantra_client.h"

using namespace quantra;

void PrintUsage(const char* program) {
    std::cerr << "Quantra JSON API Server\n\n"
              << "Usage: " << program << " <grpc_server> <http_port>\n\n"
              << "Arguments:\n"
              << "  grpc_server  - Quantra gRPC server address (e.g., localhost:50051)\n"
              << "  http_port    - HTTP port to listen on (e.g., 8080)\n";
}

int main(int argc, char** argv) {
    if (argc < 3) {
        PrintUsage(argv[0]);
        return 1;
    }
    
    std::string grpc_address = argv[1];
    int http_port = std::atoi(argv[2]);
    
    std::cout << "===========================================\n"
              << "  Quantra JSON API Server\n"
              << "===========================================\n"
              << "  gRPC backend: " << grpc_address << "\n"
              << "  HTTP port:    " << http_port << "\n"
              << "===========================================\n\n";
    
    try {
        // Initialize client
        QuantraClient client(grpc_address);
        
        // HTTP server
        crow::SimpleApp app;
        
        // Health checks
        CROW_ROUTE(app, "/")
        ([]() { return R"({"status":"ok","service":"quantra-json-api"})"; });
        
        CROW_ROUTE(app, "/health")
        ([]() { return R"({"status":"healthy"})"; });
        
        // Pricing endpoints
        CROW_ROUTE(app, "/price-fixed-rate-bond").methods("POST"_method)
        ([&](const crow::request& req) {
            auto r = client.PriceFixedRateBondJSON(req.body);
            return crow::response(r.status_code, r.body);
        });
        
        CROW_ROUTE(app, "/price-floating-rate-bond").methods("POST"_method)
        ([&](const crow::request& req) {
            auto r = client.PriceFloatingRateBondJSON(req.body);
            return crow::response(r.status_code, r.body);
        });
        
        CROW_ROUTE(app, "/price-vanilla-swap").methods("POST"_method)
        ([&](const crow::request& req) {
            auto r = client.PriceVanillaSwapJSON(req.body);
            return crow::response(r.status_code, r.body);
        });
        
        CROW_ROUTE(app, "/price-fra").methods("POST"_method)
        ([&](const crow::request& req) {
            auto r = client.PriceFRAJSON(req.body);
            return crow::response(r.status_code, r.body);
        });
        
        CROW_ROUTE(app, "/price-cap-floor").methods("POST"_method)
        ([&](const crow::request& req) {
            auto r = client.PriceCapFloorJSON(req.body);
            return crow::response(r.status_code, r.body);
        });
        
        CROW_ROUTE(app, "/price-swaption").methods("POST"_method)
        ([&](const crow::request& req) {
            auto r = client.PriceSwaptionJSON(req.body);
            return crow::response(r.status_code, r.body);
        });
        
        CROW_ROUTE(app, "/price-cds").methods("POST"_method)
        ([&](const crow::request& req) {
            auto r = client.PriceCDSJSON(req.body);
            return crow::response(r.status_code, r.body);
        });
        
        CROW_ROUTE(app, "/bootstrap-curves").methods("POST"_method)
        ([&](const crow::request& req) {
            auto r = client.BootstrapCurvesJSON(req.body);
            return crow::response(r.status_code, r.body);
        });
        
        // Print endpoints
        std::cout << "Endpoints:\n"
                  << "  POST /price-fixed-rate-bond\n"
                  << "  POST /price-floating-rate-bond\n"
                  << "  POST /price-vanilla-swap\n"
                  << "  POST /price-fra\n"
                  << "  POST /price-cap-floor\n"
                  << "  POST /price-swaption\n"
                  << "  POST /price-cds\n"
                  << "  POST /bootstrap-curves\n"
                  << "  GET  /health\n\n"
                  << "Starting server...\n";
        
        app.port(http_port).multithreaded().run();
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    
    return 0;
}