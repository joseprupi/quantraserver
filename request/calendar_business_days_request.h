#ifndef QUANTRASERVER_CALENDAR_BUSINESS_DAYS_REQUEST_H
#define QUANTRASERVER_CALENDAR_BUSINESS_DAYS_REQUEST_H

#include "flatbuffers/grpc.h"
#include "calendar_business_days_request_generated.h"
#include "calendar_business_days_response_generated.h"

class CalendarBusinessDaysRequestHandler {
public:
    flatbuffers::Offset<quantra::CalendarBusinessDaysResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::CalendarBusinessDaysRequest* request) const;
};

#endif // QUANTRASERVER_CALENDAR_BUSINESS_DAYS_REQUEST_H
