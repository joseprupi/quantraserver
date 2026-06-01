#ifndef QUANTRASERVER_CALENDAR_HOLIDAYS_HANDLER_H
#define QUANTRASERVER_CALENDAR_HOLIDAYS_HANDLER_H

#include "call_data_base.h"
#include "product_endpoint.h"
#include "product_registry.h"
#include "calendar_holidays_mapper.h"
#include "calendar_holidays_evaluator.h"
#include "calendar_holidays_request_generated.h"
#include "calendar_holidays_response_generated.h"

using quantra::CalendarHolidaysRequest;
using quantra::CalendarHolidaysResponse;
using quantra::CalendarHolidaysResponseBuilder;

/// Generic endpoint binding for the CalendarHolidays utility. The
/// FlatBuffers→domain→QuantLib→FlatBuffers glue lives in ProductEndpoint;
/// the handler only picks the four types and registers itself.
using CalendarHolidaysEndpoint = quantra::ProductEndpoint<
    CalendarHolidaysRequest,
    CalendarHolidaysResponse,
    quantra::CalendarHolidaysMapper,
    quantra::CalendarHolidaysEvaluator>;

class CalendarHolidaysData : public CallDataGeneric<
    CalendarHolidaysRequest,
    CalendarHolidaysEndpoint,
    CalendarHolidaysResponse,
    CalendarHolidaysResponseBuilder>
{
public:
    CalendarHolidaysData(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq)
        : CallDataGeneric(service, cq) {
    }

    void RequestCall() override {
        service_->RequestCalendarHolidays(
            &ctx_, &request_msg, &responder_, cq_, cq_, this);
    }

    void CreateService(QuantraServer::AsyncService* service, grpc::ServerCompletionQueue* cq) override {
        auto handler = new CalendarHolidaysData(service, cq);
        handler->start();
    }
};

REGISTER_PRODUCT(CalendarHolidays, CalendarHolidaysData);

#endif // QUANTRASERVER_CALENDAR_HOLIDAYS_HANDLER_H
