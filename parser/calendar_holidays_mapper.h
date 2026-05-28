#ifndef QUANTRA_CALENDAR_HOLIDAYS_MAPPER_H
#define QUANTRA_CALENDAR_HOLIDAYS_MAPPER_H

/**
 * CalendarHolidaysMapper — the one place this product's flatbuffers live.
 * Decodes a CalendarHolidaysRequest into plain inputs and serializes the
 * pricer result back into a CalendarHolidaysResponse.
 */

#include "flatbuffers/grpc.h"

#include "calendar_holidays_pricer.h"
#include "calendar_holidays_request_generated.h"
#include "calendar_holidays_response_generated.h"

namespace quantra {

class CalendarHolidaysMapper {
public:
    CalendarHolidaysInputs toInputs(
        const quantra::CalendarHolidaysRequest* req) const;

    flatbuffers::Offset<quantra::CalendarHolidaysResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CalendarHolidaysResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CALENDAR_HOLIDAYS_MAPPER_H
