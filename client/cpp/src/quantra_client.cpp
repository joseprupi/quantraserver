/**
 * @file quantra_client.cpp
 * @brief QuantraClient implementation
 */

#include "quantra_client.h"
#include "product_catalog.h"

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

int GrpcToHttpStatus(grpc::StatusCode code) {
    switch (code) {
        case grpc::StatusCode::INVALID_ARGUMENT: return 400;
        case grpc::StatusCode::UNAUTHENTICATED: return 401;
        case grpc::StatusCode::PERMISSION_DENIED: return 403;
        case grpc::StatusCode::NOT_FOUND: return 404;
        case grpc::StatusCode::ALREADY_EXISTS: return 409;
        case grpc::StatusCode::RESOURCE_EXHAUSTED: return 429;
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
    std::string code_name = codeName(status.error_code());
    oss << R"({"error": "gRPC error", "code": )" << status.error_code()
        << R"(, "code_name": ")" << JsonEscape(code_name)
        << R"(", "message": ")" << JsonEscape(details) << R"("})";
    return {false, GrpcToHttpStatus(status.error_code()), oss.str()};
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

    // Generic JSON call handler
    template<typename Request, typename Response>
    JsonResponse CallJSON(
        ProductType type,
        const std::string& json,
        grpc::Status (QuantraServer::Stub::*rpc)(
            grpc::ClientContext*,
            const flatbuffers::grpc::Message<Request>&,
            flatbuffers::grpc::Message<Response>*
        )
    ) {
        try {
            auto builder = json_parser_->ParseRequest(type, json);
            auto request = builder->template ReleaseMessage<Request>();
            if (!request.Verify()) {
                return JsonResponse::BadRequest("Invalid FlatBuffers request");
            }
            grpc::ClientContext context;
            SetDefaultDeadline(context);
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

JsonResponse QuantraClient::PriceFixedRateBondJSON(const std::string& json) {
    return impl_->CallJSON<PriceFixedRateBondRequest, PriceFixedRateBondResponse>(
        ProductType::FixedRateBond, json, &QuantraServer::Stub::PriceFixedRateBond
    );
}

JsonResponse QuantraClient::PriceFloatingRateBondJSON(const std::string& json) {
    return impl_->CallJSON<PriceFloatingRateBondRequest, PriceFloatingRateBondResponse>(
        ProductType::FloatingRateBond, json, &QuantraServer::Stub::PriceFloatingRateBond
    );
}

JsonResponse QuantraClient::PriceVanillaSwapJSON(const std::string& json) {
    return impl_->CallJSON<PriceVanillaSwapRequest, PriceVanillaSwapResponse>(
        ProductType::VanillaSwap, json, &QuantraServer::Stub::PriceVanillaSwap
    );
}

JsonResponse QuantraClient::PriceZeroCouponInflationSwapJSON(const std::string& json) {
    return impl_->CallJSON<PriceZeroCouponInflationSwapRequest, PriceZeroCouponInflationSwapResponse>(
        ProductType::ZeroCouponInflationSwap, json, &QuantraServer::Stub::PriceZeroCouponInflationSwap
    );
}

JsonResponse QuantraClient::PriceYearOnYearInflationSwapJSON(const std::string& json) {
    return impl_->CallJSON<PriceYearOnYearInflationSwapRequest, PriceYearOnYearInflationSwapResponse>(
        ProductType::YearOnYearInflationSwap, json, &QuantraServer::Stub::PriceYearOnYearInflationSwap
    );
}

JsonResponse QuantraClient::PriceOisSwapJSON(const std::string& json) {
    return impl_->CallJSON<PriceOisSwapRequest, PriceOisSwapResponse>(
        ProductType::OisSwap, json, &QuantraServer::Stub::PriceOisSwap
    );
}

JsonResponse QuantraClient::PriceBasisSwapJSON(const std::string& json) {
    return impl_->CallJSON<PriceBasisSwapRequest, PriceBasisSwapResponse>(
        ProductType::BasisSwap, json, &QuantraServer::Stub::PriceBasisSwap
    );
}

JsonResponse QuantraClient::PriceFRAJSON(const std::string& json) {
    return impl_->CallJSON<PriceFRARequest, PriceFRAResponse>(
        ProductType::FRA, json, &QuantraServer::Stub::PriceFRA
    );
}

JsonResponse QuantraClient::PriceCapFloorJSON(const std::string& json) {
    return impl_->CallJSON<PriceCapFloorRequest, PriceCapFloorResponse>(
        ProductType::CapFloor, json, &QuantraServer::Stub::PriceCapFloor
    );
}

JsonResponse QuantraClient::PriceSwaptionJSON(const std::string& json) {
    return impl_->CallJSON<PriceSwaptionRequest, PriceSwaptionResponse>(
        ProductType::Swaption, json, &QuantraServer::Stub::PriceSwaption
    );
}

JsonResponse QuantraClient::PriceCDSJSON(const std::string& json) {
    return impl_->CallJSON<PriceCDSRequest, PriceCDSResponse>(
        ProductType::CDS, json, &QuantraServer::Stub::PriceCDS
    );
}

JsonResponse QuantraClient::BootstrapCurvesJSON(const std::string& json) {
    return impl_->CallJSON<BootstrapCurvesRequest, BootstrapCurvesResponse>(
        ProductType::BootstrapCurves, json, &QuantraServer::Stub::BootstrapCurves
    );
}

JsonResponse QuantraClient::BootstrapInflationCurvesJSON(const std::string& json) {
    return impl_->CallJSON<BootstrapInflationCurvesRequest, BootstrapInflationCurvesResponse>(
        ProductType::BootstrapInflationCurves, json, &QuantraServer::Stub::BootstrapInflationCurves
    );
}

JsonResponse QuantraClient::SampleVolSurfacesJSON(const std::string& json) {
    return impl_->CallJSON<SampleVolSurfacesRequest, SampleVolSurfacesResponse>(
        ProductType::SampleVolSurfaces, json, &QuantraServer::Stub::SampleVolSurfaces
    );
}

JsonResponse QuantraClient::CalendarBusinessDaysJSON(const std::string& json) {
    return impl_->CallJSON<CalendarBusinessDaysRequest, CalendarBusinessDaysResponse>(
        ProductType::CalendarBusinessDays, json, &QuantraServer::Stub::CalendarBusinessDays
    );
}

JsonResponse QuantraClient::CalendarHolidaysJSON(const std::string& json) {
    return impl_->CallJSON<CalendarHolidaysRequest, CalendarHolidaysResponse>(
        ProductType::CalendarHolidays, json, &QuantraServer::Stub::CalendarHolidays
    );
}

JsonResponse QuantraClient::CalendarAdvanceJSON(const std::string& json) {
    return impl_->CallJSON<CalendarAdvanceRequest, CalendarAdvanceResponse>(
        ProductType::CalendarAdvance, json, &QuantraServer::Stub::CalendarAdvance
    );
}

JsonResponse QuantraClient::CalibrateSwaptionModelJSON(const std::string& json) {
    return impl_->CallJSON<CalibrateSwaptionModelRequest, CalibrateSwaptionModelResponse>(
        ProductType::CalibrateSwaptionModel, json, &QuantraServer::Stub::CalibrateSwaptionModel
    );
}

JsonResponse QuantraClient::PriceEquityOptionJSON(const std::string& json) {
    return impl_->CallJSON<PriceEquityOptionRequest, PriceEquityOptionResponse>(
        ProductType::EquityOption, json, &QuantraServer::Stub::PriceEquityOption
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

grpc::Status QuantraClient::PriceEquityOption(
    const Message<PriceEquityOptionRequest>& request,
    Message<PriceEquityOptionResponse>* response
) {
    grpc::ClientContext context;
    SetDefaultDeadline(context);
    return impl_->GetStub()->PriceEquityOption(&context, request, response);
}

} // namespace quantra