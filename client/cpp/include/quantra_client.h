/**
 * @file quantra_client.h
 * @brief Quantra gRPC Client Library - Main Header
 * 
 * A clean, modular C++ client for the Quantra pricing engine.
 * Supports both native FlatBuffers (max performance) and JSON (convenience) APIs.
 * 
 * Usage:
 *   #include "quantra_client.h"
 *   
 *   quantra::QuantraClient client("localhost:50051");
 *   
 *   // JSON API
 *   auto result = client.PriceFixedRateBondJSON(json_string);
 *   
 *   // Native API
 *   auto response = client.PriceFixedRateBond(request);
 */

#ifndef QUANTRA_CLIENT_H
#define QUANTRA_CLIENT_H

#include <memory>
#include <string>
#include <sstream>
#include <chrono>

#include <grpcpp/grpcpp.h>
#include "flatbuffers/idl.h"
#include "flatbuffers/util.h"
#include "flatbuffers/grpc.h"

#include "quantraserver.grpc.fb.h"

// Generated response types
#include "fixed_rate_bond_response_generated.h"
#include "floating_rate_bond_response_generated.h"
#include "vanilla_swap_response_generated.h"
#include "ois_swap_response_generated.h"
#include "basis_swap_response_generated.h"
#include "fra_response_generated.h"
#include "cap_floor_response_generated.h"
#include "swaption_response_generated.h"
#include "cds_response_generated.h"
#include "bootstrap_curves_response_generated.h"
#include "sample_vol_surfaces_response_generated.h"
#include "calendar_business_days_response_generated.h"
#include "calendar_holidays_response_generated.h"
#include "calendar_advance_response_generated.h"

namespace quantra {

// =============================================================================
// Configuration - Edit these paths for your environment
// =============================================================================

struct Config {
    static constexpr const char* FBS_INCLUDE_DIR = "/workspace/flatbuffers/fbs";
    static constexpr const char* FBS_DIR = "/workspace/flatbuffers/fbs";
};

// =============================================================================
// Product Types
// =============================================================================

enum class ProductType {
    FixedRateBond,
    FloatingRateBond,
    VanillaSwap,
    OisSwap,
    BasisSwap,
    FRA,
    CapFloor,
    Swaption,
    CDS,
    BootstrapCurves,
    SampleVolSurfaces,
    CalendarBusinessDays,
    CalendarHolidays,
    CalendarAdvance
};

const char* ProductTypeToString(ProductType type);

// =============================================================================
// JSON Response Structure
// =============================================================================

struct JsonResponse {
    bool ok;
    int status_code;
    std::string body;
    
    static JsonResponse Success(const std::string& json);
    static JsonResponse BadRequest(const std::string& error);
    static JsonResponse ServerError(const std::string& error);
    static JsonResponse GrpcError(const grpc::Status& status);
};

// =============================================================================
// JSON Parser - Handles JSON <-> FlatBuffers conversion
// =============================================================================

class JsonParser {
public:
    JsonParser();
    ~JsonParser();
    
    // Request: JSON -> FlatBuffers
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> ParseRequest(ProductType type, const std::string& json);
    
    // Response: FlatBuffers -> JSON
    std::string GenerateResponse(ProductType type, const uint8_t* buffer);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

// =============================================================================
// Quantra Client - Main API
// =============================================================================

class QuantraClient {
public:
    explicit QuantraClient(const std::string& address);
    QuantraClient(const std::string& address, bool use_tls);
    ~QuantraClient();
    
    // -------------------------------------------------------------------------
    // JSON API - Easy to use, works with any language via HTTP
    // -------------------------------------------------------------------------
    
    JsonResponse PriceFixedRateBondJSON(const std::string& json);
    JsonResponse PriceFloatingRateBondJSON(const std::string& json);
    JsonResponse PriceVanillaSwapJSON(const std::string& json);
    JsonResponse PriceOisSwapJSON(const std::string& json);
    JsonResponse PriceBasisSwapJSON(const std::string& json);
    JsonResponse PriceFRAJSON(const std::string& json);
    JsonResponse PriceCapFloorJSON(const std::string& json);
    JsonResponse PriceSwaptionJSON(const std::string& json);
    JsonResponse PriceCDSJSON(const std::string& json);
    JsonResponse BootstrapCurvesJSON(const std::string& json);
    JsonResponse SampleVolSurfacesJSON(const std::string& json);
    JsonResponse CalendarBusinessDaysJSON(const std::string& json);
    JsonResponse CalendarHolidaysJSON(const std::string& json);
    JsonResponse CalendarAdvanceJSON(const std::string& json);
    
    // -------------------------------------------------------------------------
    // Native FlatBuffers API - Maximum performance
    // -------------------------------------------------------------------------
    
    template<typename T>
    using Message = flatbuffers::grpc::Message<T>;
    
    grpc::Status PriceFixedRateBond(
        const Message<PriceFixedRateBondRequest>& request,
        Message<PriceFixedRateBondResponse>* response);
    
    grpc::Status PriceFloatingRateBond(
        const Message<PriceFloatingRateBondRequest>& request,
        Message<PriceFloatingRateBondResponse>* response);
    
    grpc::Status PriceVanillaSwap(
        const Message<PriceVanillaSwapRequest>& request,
        Message<PriceVanillaSwapResponse>* response);

    grpc::Status PriceOisSwap(
        const Message<PriceOisSwapRequest>& request,
        Message<PriceOisSwapResponse>* response);

    grpc::Status PriceBasisSwap(
        const Message<PriceBasisSwapRequest>& request,
        Message<PriceBasisSwapResponse>* response);
    
    grpc::Status PriceFRA(
        const Message<PriceFRARequest>& request,
        Message<PriceFRAResponse>* response);
    
    grpc::Status PriceCapFloor(
        const Message<PriceCapFloorRequest>& request,
        Message<PriceCapFloorResponse>* response);
    
    grpc::Status PriceSwaption(
        const Message<PriceSwaptionRequest>& request,
        Message<PriceSwaptionResponse>* response);
    
    grpc::Status PriceCDS(
        const Message<PriceCDSRequest>& request,
        Message<PriceCDSResponse>* response);
    
    grpc::Status BootstrapCurves(
        const Message<BootstrapCurvesRequest>& request,
        Message<BootstrapCurvesResponse>* response);

    grpc::Status SampleVolSurfaces(
        const Message<SampleVolSurfacesRequest>& request,
        Message<SampleVolSurfacesResponse>* response);

    grpc::Status CalendarBusinessDays(
        const Message<CalendarBusinessDaysRequest>& request,
        Message<CalendarBusinessDaysResponse>* response);

    grpc::Status CalendarHolidays(
        const Message<CalendarHolidaysRequest>& request,
        Message<CalendarHolidaysResponse>* response);

    grpc::Status CalendarAdvance(
        const Message<CalendarAdvanceRequest>& request,
        Message<CalendarAdvanceResponse>* response);
    
    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------
    
    std::shared_ptr<QuantraServer::Stub> GetStub();
    std::string GetGrpcTarget() const;
    grpc_connectivity_state GetChannelState(bool try_to_connect = false) const;
    bool WaitForChannelReady(std::chrono::milliseconds timeout) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace quantra

#endif // QUANTRA_CLIENT_H