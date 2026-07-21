/**
 * @file quantra_client.cpp
 * @brief QuantraClient implementation
 */

#include "quantra_client.h"
#include "product_catalog.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <iostream>
#include <string_view>
#include <cstdio>

namespace quantra {

namespace {

constexpr std::chrono::milliseconds kDefaultRpcDeadline{10000};

void SetDefaultDeadline(grpc::ClientContext& context) {
    context.set_deadline(std::chrono::system_clock::now() + kDefaultRpcDeadline);
}

std::string JsonEscape(std::string_view input) {
    std::string out;
    out.reserve(input.size() + 8);
    for (char c : input) {
        switch (c) {
            case '\"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

bool ContainsCaseInsensitive(const std::string& haystack, std::string_view needle) {
    auto to_lower = [](unsigned char c) { return static_cast<char>(std::tolower(c)); };
    std::string h(haystack.size(), '\0');
    std::transform(haystack.begin(), haystack.end(), h.begin(), to_lower);
    std::string n(needle.size(), '\0');
    std::transform(needle.begin(), needle.end(), n.begin(), to_lower);
    return h.find(n) != std::string::npos;
}

int GrpcToHttpStatus(const grpc::Status& status) {
    switch (status.error_code()) {
        case grpc::StatusCode::INVALID_ARGUMENT: return 400;
        case grpc::StatusCode::UNAUTHENTICATED: return 401;
        case grpc::StatusCode::PERMISSION_DENIED: return 403;
        case grpc::StatusCode::NOT_FOUND: return 404;
        case grpc::StatusCode::ALREADY_EXISTS: return 409;
        case grpc::StatusCode::RESOURCE_EXHAUSTED:
            // The only known RESOURCE_EXHAUSTED source is gRPC's message-size
            // cap ("Received message larger than max ..."), which is a
            // payload-size problem, not a rate-limit -> 413. Anything else
            // maps to 429.
            if (ContainsCaseInsensitive(status.error_message(), "larger than max") ||
                ContainsCaseInsensitive(status.error_message(), "message too large")) {
                return 413;
            }
            return 429;
        // ABORTED carries QuantLib/engine exceptions raised by well-formed but
        // unpriceable client inputs (bad dates, degenerate schedules, ...).
        // Those are client-data faults, not server faults -> 422
        // Unprocessable Entity.
        case grpc::StatusCode::ABORTED: return 422;
        case grpc::StatusCode::UNIMPLEMENTED: return 501;
        case grpc::StatusCode::UNAVAILABLE: return 503;
        case grpc::StatusCode::DEADLINE_EXCEEDED: return 504;
        case grpc::StatusCode::OK: return 200;
        default: return 500;
    }
}

} // namespace

// =============================================================================
// JsonResponse Implementation
// =============================================================================

JsonResponse JsonResponse::Success(const std::string& json) {
    return {true, 200, json};
}

JsonResponse JsonResponse::BadRequest(const std::string& error) {
    return {false, 400, R"({"error": ")" + JsonEscape(error) + R"("})"};
}

JsonResponse JsonResponse::ServerError(const std::string& error) {
    return {false, 500, R"({"error": ")" + JsonEscape(error) + R"("})"};
}

JsonResponse JsonResponse::GrpcError(const grpc::Status& status) {
    auto codeName = [](grpc::StatusCode code) -> const char* {
        switch (code) {
            case grpc::StatusCode::OK: return "OK";
            case grpc::StatusCode::CANCELLED: return "CANCELLED";
            case grpc::StatusCode::UNKNOWN: return "UNKNOWN";
            case grpc::StatusCode::INVALID_ARGUMENT: return "INVALID_ARGUMENT";
            case grpc::StatusCode::DEADLINE_EXCEEDED: return "DEADLINE_EXCEEDED";
            case grpc::StatusCode::NOT_FOUND: return "NOT_FOUND";
            case grpc::StatusCode::ALREADY_EXISTS: return "ALREADY_EXISTS";
            case grpc::StatusCode::PERMISSION_DENIED: return "PERMISSION_DENIED";
            case grpc::StatusCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
            case grpc::StatusCode::FAILED_PRECONDITION: return "FAILED_PRECONDITION";
            case grpc::StatusCode::ABORTED: return "ABORTED";
            case grpc::StatusCode::OUT_OF_RANGE: return "OUT_OF_RANGE";
            case grpc::StatusCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
            case grpc::StatusCode::INTERNAL: return "INTERNAL";
            case grpc::StatusCode::UNAVAILABLE: return "UNAVAILABLE";
            case grpc::StatusCode::DATA_LOSS: return "DATA_LOSS";
            case grpc::StatusCode::UNAUTHENTICATED: return "UNAUTHENTICATED";
            default: return "UNKNOWN_CODE";
        }
    };

    std::ostringstream oss;
    std::string details = status.error_details();
    if (details.empty()) {
        details = status.error_message();
    }
    // The documented `error` field carries the real cause (same text as
    // `message`); it only falls back to the constant "gRPC error" when the
    // status carries no details or message at all.
    // `code`, `code_name` and `message` are kept for backward compatibility.
    std::string error_text = details.empty() ? "gRPC error" : details;
    std::string code_name = codeName(status.error_code());
    oss << R"({"error": ")" << JsonEscape(error_text)
        << R"(", "code": )" << status.error_code()
        << R"(, "code_name": ")" << JsonEscape(code_name)
        << R"(", "message": ")" << JsonEscape(details) << R"("})";
    return {false, GrpcToHttpStatus(status), oss.str()};
}

// =============================================================================
// QuantraClient Implementation (PIMPL)
// =============================================================================

class QuantraClient::Impl {
public:
    Impl(const std::string& address, bool use_tls) {
        backend_address_ = address;
        InitChannel(address, use_tls);
        json_parser_ = std::make_unique<JsonParser>();
    }
    
    std::shared_ptr<QuantraServer::Stub> GetStub() { return stub_; }
    std::string GetGrpcTarget() const { return backend_address_; }
    grpc_connectivity_state GetChannelState(bool try_to_connect) const {
        return channel_->GetState(try_to_connect);
    }
    bool WaitForChannelReady(std::chrono::milliseconds timeout) const {
        auto deadline = std::chrono::system_clock::now() + timeout;
        return channel_->WaitForConnected(deadline);
    }
    JsonParser& GetParser() { return *json_parser_; }

    // Generic JSON call handler. `request_id` (already sanitized by the HTTP
    // layer) is forwarded to the backend as `x-request-id` gRPC metadata so
    // engine log lines correlate with the caller's id.
    template<typename Request, typename Response>
    JsonResponse CallJSON(
        ProductType type,
        const std::string& json,
        grpc::Status (QuantraServer::Stub::*rpc)(
            grpc::ClientContext*,
            const flatbuffers::grpc::Message<Request>&,
            flatbuffers::grpc::Message<Response>*
        ),
        const std::string& request_id = ""
    ) {
        try {
            auto builder = json_parser_->ParseRequest(type, json);
            auto request = builder->template ReleaseMessage<Request>();
            if (!request.Verify()) {
                return JsonResponse::BadRequest("Invalid FlatBuffers request");
            }
            grpc::ClientContext context;
            SetDefaultDeadline(context);
            if (!request_id.empty()) {
                context.AddMetadata("x-request-id", request_id);
            }
            flatbuffers::grpc::Message<Response> response;
            
            grpc::Status status = (stub_.get()->*rpc)(&context, request, &response);
            
            if (!status.ok()) {
                std::cerr
                    << "[QuantraClient] gRPC call failed"
                    << " product=" << ProductTypeToString(type)
                    << " backend=" << backend_address_
                    << " code=" << static_cast<int>(status.error_code())
                    << " message=\"" << status.error_message() << "\""
                    << " details=\"" << status.error_details() << "\""
                    << " debug=\"" << context.debug_error_string() << "\""
                    << std::endl;
                if (status.error_code() == grpc::StatusCode::UNIMPLEMENTED) {
                    std::cerr
                        << "[QuantraClient] hint: backend does not implement this RPC."
                        << " Ensure json_server points to the correct sync_server binary/version."
                        << std::endl;
                }
                return JsonResponse::GrpcError(status);
            }
            
            if (!response.data()) {
                return JsonResponse::ServerError("Empty response from server");
            }

            if (!response.Verify()) {
                return JsonResponse::ServerError("Invalid response from server");
            }
            
            return JsonResponse::Success(
                json_parser_->GenerateResponse(type, response.data())
            );
            
        } catch (const JsonParseException& e) {
            return JsonResponse::BadRequest(e.what());
        } catch (const JsonRuntimeException& e) {
            return JsonResponse::ServerError(e.what());
        } catch (const std::exception& e) {
            return JsonResponse::ServerError(e.what());
        }
    }

private:
    std::shared_ptr<grpc::Channel> channel_;
    std::shared_ptr<QuantraServer::Stub> stub_;
    std::unique_ptr<JsonParser> json_parser_;
    std::string backend_address_;
    
    void InitChannel(const std::string& address, bool use_tls) {
        grpc::ChannelArguments args;
        args.SetMaxReceiveMessageSize(100 * 1024 * 1024);
        args.SetMaxSendMessageSize(100 * 1024 * 1024);
        
        std::shared_ptr<grpc::ChannelCredentials> creds;
        if (use_tls) {
            creds = grpc::SslCredentials(grpc::SslCredentialsOptions());
        } else {
            creds = grpc::InsecureChannelCredentials();
        }
        
        channel_ = grpc::CreateCustomChannel(address, creds, args);
        stub_ = QuantraServer::NewStub(channel_);
    }
};

// =============================================================================
// QuantraClient Public Interface
// =============================================================================

QuantraClient::QuantraClient(const std::string& address) 
    : impl_(std::make_unique<Impl>(address, false)) {}

QuantraClient::QuantraClient(const std::string& address, bool use_tls)
    : impl_(std::make_unique<Impl>(address, use_tls)) {}

QuantraClient::~QuantraClient() = default;

std::shared_ptr<QuantraServer::Stub> QuantraClient::GetStub() {
    return impl_->GetStub();
}

std::string QuantraClient::GetGrpcTarget() const {
    return impl_->GetGrpcTarget();
}

grpc_connectivity_state QuantraClient::GetChannelState(bool try_to_connect) const {
    return impl_->GetChannelState(try_to_connect);
}

bool QuantraClient::WaitForChannelReady(std::chrono::milliseconds timeout) const {
    return impl_->WaitForChannelReady(timeout);
}

// =============================================================================
// JSON API Implementation
// =============================================================================

JsonResponse QuantraClient::PriceFixedRateBondJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceFixedRateBondRequest, PriceFixedRateBondResponse>(
        ProductType::FixedRateBond, json, &QuantraServer::Stub::PriceFixedRateBond, request_id
    );
}

JsonResponse QuantraClient::PriceFloatingRateBondJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceFloatingRateBondRequest, PriceFloatingRateBondResponse>(
        ProductType::FloatingRateBond, json, &QuantraServer::Stub::PriceFloatingRateBond, request_id
    );
}

JsonResponse QuantraClient::PriceVanillaSwapJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceVanillaSwapRequest, PriceVanillaSwapResponse>(
        ProductType::VanillaSwap, json, &QuantraServer::Stub::PriceVanillaSwap, request_id
    );
}

JsonResponse QuantraClient::PriceZeroCouponInflationSwapJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceZeroCouponInflationSwapRequest, PriceZeroCouponInflationSwapResponse>(
        ProductType::ZeroCouponInflationSwap, json, &QuantraServer::Stub::PriceZeroCouponInflationSwap, request_id
    );
}

JsonResponse QuantraClient::PriceYearOnYearInflationSwapJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceYearOnYearInflationSwapRequest, PriceYearOnYearInflationSwapResponse>(
        ProductType::YearOnYearInflationSwap, json, &QuantraServer::Stub::PriceYearOnYearInflationSwap, request_id
    );
}

JsonResponse QuantraClient::PriceOisSwapJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceOisSwapRequest, PriceOisSwapResponse>(
        ProductType::OisSwap, json, &QuantraServer::Stub::PriceOisSwap, request_id
    );
}

JsonResponse QuantraClient::PriceBasisSwapJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceBasisSwapRequest, PriceBasisSwapResponse>(
        ProductType::BasisSwap, json, &QuantraServer::Stub::PriceBasisSwap, request_id
    );
}

JsonResponse QuantraClient::PriceFRAJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceFRARequest, PriceFRAResponse>(
        ProductType::FRA, json, &QuantraServer::Stub::PriceFRA, request_id
    );
}

JsonResponse QuantraClient::PriceCapFloorJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceCapFloorRequest, PriceCapFloorResponse>(
        ProductType::CapFloor, json, &QuantraServer::Stub::PriceCapFloor, request_id
    );
}

JsonResponse QuantraClient::PriceSwaptionJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceSwaptionRequest, PriceSwaptionResponse>(
        ProductType::Swaption, json, &QuantraServer::Stub::PriceSwaption, request_id
    );
}

JsonResponse QuantraClient::PriceCDSJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceCDSRequest, PriceCDSResponse>(
        ProductType::CDS, json, &QuantraServer::Stub::PriceCDS, request_id
    );
}

JsonResponse QuantraClient::BootstrapCurvesJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<BootstrapCurvesRequest, BootstrapCurvesResponse>(
        ProductType::BootstrapCurves, json, &QuantraServer::Stub::BootstrapCurves, request_id
    );
}

JsonResponse QuantraClient::BootstrapInflationCurvesJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<BootstrapInflationCurvesRequest, BootstrapInflationCurvesResponse>(
        ProductType::BootstrapInflationCurves, json, &QuantraServer::Stub::BootstrapInflationCurves, request_id
    );
}

JsonResponse QuantraClient::SampleVolSurfacesJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<SampleVolSurfacesRequest, SampleVolSurfacesResponse>(
        ProductType::SampleVolSurfaces, json, &QuantraServer::Stub::SampleVolSurfaces, request_id
    );
}

JsonResponse QuantraClient::CalendarBusinessDaysJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<CalendarBusinessDaysRequest, CalendarBusinessDaysResponse>(
        ProductType::CalendarBusinessDays, json, &QuantraServer::Stub::CalendarBusinessDays, request_id
    );
}

JsonResponse QuantraClient::CalendarHolidaysJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<CalendarHolidaysRequest, CalendarHolidaysResponse>(
        ProductType::CalendarHolidays, json, &QuantraServer::Stub::CalendarHolidays, request_id
    );
}

JsonResponse QuantraClient::CalendarAdvanceJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<CalendarAdvanceRequest, CalendarAdvanceResponse>(
        ProductType::CalendarAdvance, json, &QuantraServer::Stub::CalendarAdvance, request_id
    );
}

JsonResponse QuantraClient::CalibrateSwaptionModelJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<CalibrateSwaptionModelRequest, CalibrateSwaptionModelResponse>(
        ProductType::CalibrateSwaptionModel, json, &QuantraServer::Stub::CalibrateSwaptionModel, request_id
    );
}

JsonResponse QuantraClient::CalibrateSwaptionVolJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<CalibrateSwaptionVolRequest, CalibrateSwaptionVolResponse>(
        ProductType::CalibrateSwaptionVol, json, &QuantraServer::Stub::CalibrateSwaptionVol, request_id
    );
}

JsonResponse QuantraClient::PriceEquityOptionJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceEquityOptionRequest, PriceEquityOptionResponse>(
        ProductType::EquityOption, json, &QuantraServer::Stub::PriceEquityOption, request_id
    );
}

JsonResponse QuantraClient::PriceZeroCouponBondJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceZeroCouponBondRequest, PriceZeroCouponBondResponse>(
        ProductType::ZeroCouponBond, json, &QuantraServer::Stub::PriceZeroCouponBond, request_id
    );
}

JsonResponse QuantraClient::PriceZeroCouponSwapJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceZeroCouponSwapRequest, PriceZeroCouponSwapResponse>(
        ProductType::ZeroCouponSwap, json, &QuantraServer::Stub::PriceZeroCouponSwap, request_id
    );
}

JsonResponse QuantraClient::PriceYearOnYearInflationCapFloorJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceYearOnYearInflationCapFloorRequest, PriceYearOnYearInflationCapFloorResponse>(
        ProductType::YearOnYearInflationCapFloor, json, &QuantraServer::Stub::PriceYearOnYearInflationCapFloor, request_id
    );
}

JsonResponse QuantraClient::PriceCallableFixedRateBondJSON(const std::string& json, const std::string& request_id) {
    return impl_->CallJSON<PriceCallableFixedRateBondRequest, PriceCallableFixedRateBondResponse>(
        ProductType::CallableFixedRateBond, json, &QuantraServer::Stub::PriceCallableFixedRateBond, request_id
    );
}

// =============================================================================
// Native FlatBuffers API Implementation
// =============================================================================

grpc::Status QuantraClient::PriceFixedRateBond(
    const Message<PriceFixedRateBondRequest>& request,
    Message<PriceFixedRateBondResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceFixedRateBond(&context, request, response);
}

grpc::Status QuantraClient::PriceFloatingRateBond(
    const Message<PriceFloatingRateBondRequest>& request,
    Message<PriceFloatingRateBondResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceFloatingRateBond(&context, request, response);
}

grpc::Status QuantraClient::PriceVanillaSwap(
    const Message<PriceVanillaSwapRequest>& request,
    Message<PriceVanillaSwapResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceVanillaSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceZeroCouponInflationSwap(
    const Message<PriceZeroCouponInflationSwapRequest>& request,
    Message<PriceZeroCouponInflationSwapResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceZeroCouponInflationSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceYearOnYearInflationSwap(
    const Message<PriceYearOnYearInflationSwapRequest>& request,
    Message<PriceYearOnYearInflationSwapResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceYearOnYearInflationSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceOisSwap(
    const Message<PriceOisSwapRequest>& request,
    Message<PriceOisSwapResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceOisSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceBasisSwap(
    const Message<PriceBasisSwapRequest>& request,
    Message<PriceBasisSwapResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceBasisSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceFRA(
    const Message<PriceFRARequest>& request,
    Message<PriceFRAResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceFRA(&context, request, response);
}

grpc::Status QuantraClient::PriceCapFloor(
    const Message<PriceCapFloorRequest>& request,
    Message<PriceCapFloorResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceCapFloor(&context, request, response);
}

grpc::Status QuantraClient::PriceSwaption(
    const Message<PriceSwaptionRequest>& request,
    Message<PriceSwaptionResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceSwaption(&context, request, response);
}

grpc::Status QuantraClient::PriceCDS(
    const Message<PriceCDSRequest>& request,
    Message<PriceCDSResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceCDS(&context, request, response);
}

grpc::Status QuantraClient::BootstrapCurves(
    const Message<BootstrapCurvesRequest>& request,
    Message<BootstrapCurvesResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->BootstrapCurves(&context, request, response);
}

grpc::Status QuantraClient::BootstrapInflationCurves(
    const Message<BootstrapInflationCurvesRequest>& request,
    Message<BootstrapInflationCurvesResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->BootstrapInflationCurves(&context, request, response);
}

grpc::Status QuantraClient::SampleVolSurfaces(
    const Message<SampleVolSurfacesRequest>& request,
    Message<SampleVolSurfacesResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->SampleVolSurfaces(&context, request, response);
}

grpc::Status QuantraClient::CalendarBusinessDays(
    const Message<CalendarBusinessDaysRequest>& request,
    Message<CalendarBusinessDaysResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->CalendarBusinessDays(&context, request, response);
}

grpc::Status QuantraClient::CalendarHolidays(
    const Message<CalendarHolidaysRequest>& request,
    Message<CalendarHolidaysResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->CalendarHolidays(&context, request, response);
}

grpc::Status QuantraClient::CalendarAdvance(
    const Message<CalendarAdvanceRequest>& request,
    Message<CalendarAdvanceResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->CalendarAdvance(&context, request, response);
}

grpc::Status QuantraClient::CalibrateSwaptionModel(
    const Message<CalibrateSwaptionModelRequest>& request,
    Message<CalibrateSwaptionModelResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->CalibrateSwaptionModel(&context, request, response);
}

grpc::Status QuantraClient::CalibrateSwaptionVol(
    const Message<CalibrateSwaptionVolRequest>& request,
    Message<CalibrateSwaptionVolResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->CalibrateSwaptionVol(&context, request, response);
}

grpc::Status QuantraClient::PriceEquityOption(
    const Message<PriceEquityOptionRequest>& request,
    Message<PriceEquityOptionResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceEquityOption(&context, request, response);
}

grpc::Status QuantraClient::PriceZeroCouponBond(
    const Message<PriceZeroCouponBondRequest>& request,
    Message<PriceZeroCouponBondResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceZeroCouponBond(&context, request, response);
}

grpc::Status QuantraClient::PriceZeroCouponSwap(
    const Message<PriceZeroCouponSwapRequest>& request,
    Message<PriceZeroCouponSwapResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceZeroCouponSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceYearOnYearInflationCapFloor(
    const Message<PriceYearOnYearInflationCapFloorRequest>& request,
    Message<PriceYearOnYearInflationCapFloorResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceYearOnYearInflationCapFloor(&context, request, response);
}

grpc::Status QuantraClient::PriceCallableFixedRateBond(
    const Message<PriceCallableFixedRateBondRequest>& request,
    Message<PriceCallableFixedRateBondResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceCallableFixedRateBond(&context, request, response);
}

} // namespace quantra