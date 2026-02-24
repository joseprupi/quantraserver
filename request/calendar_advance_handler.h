#ifndef QUANTRASERVER_CALENDAR_ADVANCE_HANDLER_H
#define QUANTRASERVER_CALENDAR_ADVANCE_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "calendar_advance_request.h"
#include "calendar_advance_request_generated.h"
#include "calendar_advance_response_generated.h"

using quantra::CalendarAdvanceRequest;
using quantra::CalendarAdvanceResponse;
using quantra::CalendarAdvanceResponseBuilder;

class CalendarAdvanceData : public CallDataGeneric<
    CalendarAdvanceRequest,
    CalendarAdvanceRequestHandler,
    CalendarAdvanceResponse,
    CalendarAdvanceResponseBuilder>
{
public:
    CalendarAdvanceData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {
    }

    void RequestCall() override {
        service_->RequestCalendarAdvance(
            &ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalendarAdvanceData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalendarAdvance, CalendarAdvanceData);

#endif // QUANTRASERVER_CALENDAR_ADVANCE_HANDLER_H
