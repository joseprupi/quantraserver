#ifndef QUANTRASERVER_STRUCTURED_NOTE_HANDLER_H
#define QUANTRASERVER_STRUCTURED_NOTE_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "structured_note_evaluator.h"
#include "structured_note_mapper.h"

#include "price_structured_note_request_generated.h"
#include "structured_note_response_generated.h"

using quantra::PriceStructuredNoteRequest;
using quantra::PriceStructuredNoteResponse;
using quantra::PriceStructuredNoteResponseBuilder;

/// Generic endpoint binding for the structured-note product. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using StructuredNoteEndpoint = quantra::ProductEndpoint<
    PriceStructuredNoteRequest,
    PriceStructuredNoteResponse,
    quantra::StructuredNoteMapper,
    quantra::StructuredNoteEvaluator>;

/**
 * PriceStructuredNoteData - Async handler for structured-note pricing.
 */
class PriceStructuredNoteData : public CallDataGeneric<
                                    PriceStructuredNoteRequest,
                                    StructuredNoteEndpoint,
                                    PriceStructuredNoteResponse,
                                    PriceStructuredNoteResponseBuilder> {
public:
    PriceStructuredNoteData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {}

    void RequestCall() override {
        service_->RequestPriceStructuredNote(&ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(
        QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new PriceStructuredNoteData(service, cq);
        handler->start();
    }
};

// Auto-register with the product registry
REGISTER_PRODUCT(PriceStructuredNote, PriceStructuredNoteData);

#endif // QUANTRASERVER_STRUCTURED_NOTE_HANDLER_H
