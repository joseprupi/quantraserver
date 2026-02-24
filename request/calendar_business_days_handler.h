#ifndef QUANTRASERVER_CALENDAR_BUSINESS_DAYS_HANDLER_H
#define QUANTRASERVER_CALENDAR_BUSINESS_DAYS_HANDLER_H

#include "call_data_base.h"
#include "product_registry.h"
#include "calendar_business_days_request.h"
#include "calendar_business_days_request_generated.h"
#include "calendar_business_days_response_generated.h"

using quantra::CalendarBusinessDaysRequest;
using quantra::CalendarBusinessDaysResponse;
using quantra::CalendarBusinessDaysResponseBuilder;

class CalendarBusinessDaysData : public CallDataGeneric<
    CalendarBusinessDaysRequest,
    CalendarBusinessDaysRequestHandler,
    CalendarBusinessDaysResponse,
    CalendarBusinessDaysResponseBuilder>
{
public:
    CalendarBusinessDaysData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {
    }

    void RequestCall() override {
        service_->RequestCalendarBusinessDays(
            &ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalendarBusinessDaysData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalendarBusinessDays, CalendarBusinessDaysData);

#endif // QUANTRASERVER_CALENDAR_BUSINESS_DAYS_HANDLER_H
