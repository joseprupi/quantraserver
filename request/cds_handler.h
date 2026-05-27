#ifndef QUANTRASERVER_CDS_HANDLER_H
#define QUANTRASERVER_CDS_HANDLER_H

#include "call_data_base.h"
#include "cds_mapper.h"
#include "cds_pricer.h"
#include "cds_response_generated.h"
#include "price_cds_request_generated.h"
#include "product_endpoint.h"
#include "product_registry.h"

using quantra::PriceCDSRequest;
using quantra::PriceCDSResponse;
using quantra::PriceCDSResponseBuilder;

/// Wave-A product: credit default swap. Mirrors the YYIIS/ZCIIS cutovers —
/// the generic ProductEndpoint template owns the
/// Verify → mapper.toInputs → registry → context → pricer.price →
/// mapper.toResponse glue.
using CDSEndpoint = quantra::ProductEndpoint<
    PriceCDSRequest,
    PriceCDSResponse,
    quantra::CdsMapper,
    quantra::CdsPricer>;

/// Backward-compatible alias for existing call sites (tests) that referenced
/// the legacy CDSPricingRequest type directly.
using CDSPricingRequest = CDSEndpoint;

class PriceCDSData : public CallDataGeneric<
    PriceCDSRequest,
    CDSEndpoint,
    PriceCDSResponse,
    PriceCDSResponseBuilder>
{
public:
    PriceCDSData(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq)
        : CallDataGeneric(service, cq)
    {
    }

    void RequestCall() override
    {
        service_->RequestPriceCDS(
            &ctx_, &request_msg, &responder_,
            cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService *service, grpc::ServerCompletionQueue *cq) override
    {
        auto handler = new PriceCDSData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceCDS, PriceCDSData);

#endif // QUANTRASERVER_CDS_HANDLER_H
