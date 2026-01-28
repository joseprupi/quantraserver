#ifndef QUANTRASERVER_CDS_HANDLER_H
#define QUANTRASERVER_CDS_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "cds_pricing_request.h"
#include "price_cds_request_generated.h"
#include "cds_response_generated.h"

using quantra::PriceCDSRequest;
using quantra::PriceCDSResponse;
using quantra::PriceCDSResponseBuilder;

/**
 * PriceCDSData - Async handler for CDS pricing.
 */
class PriceCDSData : public CallDataGeneric<
    PriceCDSRequest,
    CDSPricingRequest,
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