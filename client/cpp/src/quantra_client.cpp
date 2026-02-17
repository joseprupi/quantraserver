/**
 * @file quantra_client.cpp
 * @brief QuantraClient implementation
 */

#include "quantra_client.h"
#include "product_registry.h"

#include <stdexcept>
#include <iostream>

namespace quantra {

// =============================================================================
// JsonResponse Implementation
// =============================================================================

JsonResponse JsonResponse::Success(const std::string& json) {
    return {true, 200, json};
}

JsonResponse JsonResponse::BadRequest(const std::string& error) {
    return {false, 400, R"({"error": ")" + error + R"("})"};
}

JsonResponse JsonResponse::ServerError(const std::string& error) {
    return {false, 500, R"({"error": ")" + error + R"("})"};
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
        << R"(, "code_name": ")" << code_name
        << R"(", "message": ")" << details << R"("})";
    return {false, 500, oss.str()};
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
            
            return JsonResponse::Success(
                json_parser_->GenerateResponse(type, response.data())
            );
            
        } catch (const std::exception& e) {
            return JsonResponse::BadRequest(e.what());
        }
    }

private:
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
        
        stub_ = QuantraServer::NewStub(
            grpc::CreateCustomChannel(address, creds, args)
        );
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

JsonResponse QuantraClient::SampleVolSurfacesJSON(const std::string& json) {
    return impl_->CallJSON<SampleVolSurfacesRequest, SampleVolSurfacesResponse>(
        ProductType::SampleVolSurfaces, json, &QuantraServer::Stub::SampleVolSurfaces
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
    return impl_->GetStub()->PriceFixedRateBond(&context, request, response);
}

grpc::Status QuantraClient::PriceFloatingRateBond(
    const Message<PriceFloatingRateBondRequest>& request,
    Message<PriceFloatingRateBondResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceFloatingRateBond(&context, request, response);
}

grpc::Status QuantraClient::PriceVanillaSwap(
    const Message<PriceVanillaSwapRequest>& request,
    Message<PriceVanillaSwapResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceVanillaSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceOisSwap(
    const Message<PriceOisSwapRequest>& request,
    Message<PriceOisSwapResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceOisSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceBasisSwap(
    const Message<PriceBasisSwapRequest>& request,
    Message<PriceBasisSwapResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceBasisSwap(&context, request, response);
}

grpc::Status QuantraClient::PriceFRA(
    const Message<PriceFRARequest>& request,
    Message<PriceFRAResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceFRA(&context, request, response);
}

grpc::Status QuantraClient::PriceCapFloor(
    const Message<PriceCapFloorRequest>& request,
    Message<PriceCapFloorResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceCapFloor(&context, request, response);
}

grpc::Status QuantraClient::PriceSwaption(
    const Message<PriceSwaptionRequest>& request,
    Message<PriceSwaptionResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceSwaption(&context, request, response);
}

grpc::Status QuantraClient::PriceCDS(
    const Message<PriceCDSRequest>& request,
    Message<PriceCDSResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->PriceCDS(&context, request, response);
}

grpc::Status QuantraClient::BootstrapCurves(
    const Message<BootstrapCurvesRequest>& request,
    Message<BootstrapCurvesResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->BootstrapCurves(&context, request, response);
}

grpc::Status QuantraClient::SampleVolSurfaces(
    const Message<SampleVolSurfacesRequest>& request,
    Message<SampleVolSurfacesResponse>* response
) {
    grpc::ClientContext context;
    return impl_->GetStub()->SampleVolSurfaces(&context, request, response);
}

} // namespace quantra