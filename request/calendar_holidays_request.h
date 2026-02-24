#ifndef QUANTRASERVER_CALENDAR_HOLIDAYS_REQUEST_H
#define QUANTRASERVER_CALENDAR_HOLIDAYS_REQUEST_H

#include "flatbuffers/grpc.h"
#include "calendar_holidays_request_generated.h"
#include "calendar_holidays_response_generated.h"

class CalendarHolidaysRequestHandler {
public:
    flatbuffers::Offset<quantra::CalendarHolidaysResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::CalendarHolidaysRequest* request) const;
};

#endif // QUANTRASERVER_CALENDAR_HOLIDAYS_REQUEST_H
