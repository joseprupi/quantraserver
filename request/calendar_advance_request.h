#ifndef QUANTRASERVER_CALENDAR_ADVANCE_REQUEST_H
#define QUANTRASERVER_CALENDAR_ADVANCE_REQUEST_H

#include "flatbuffers/grpc.h"
#include "calendar_advance_request_generated.h"
#include "calendar_advance_response_generated.h"

class CalendarAdvanceRequestHandler {
public:
    flatbuffers::Offset<quantra::CalendarAdvanceResponse> request(
        std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
        const quantra::CalendarAdvanceRequest* request) const;
};

#endif // QUANTRASERVER_CALENDAR_ADVANCE_REQUEST_H
