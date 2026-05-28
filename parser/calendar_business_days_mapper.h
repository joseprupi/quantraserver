#ifndef QUANTRA_CALENDAR_BUSINESS_DAYS_MAPPER_H
#define QUANTRA_CALENDAR_BUSINESS_DAYS_MAPPER_H

/**
 * CalendarBusinessDaysMapper — the one place this product's flatbuffers live.
 * Decodes a CalendarBusinessDaysRequest into plain inputs and serializes the
 * pricer result back into a CalendarBusinessDaysResponse.
 */

#include "flatbuffers/grpc.h"

#include "calendar_business_days_pricer.h"
#include "calendar_business_days_request_generated.h"
#include "calendar_business_days_response_generated.h"

namespace quantra {

class CalendarBusinessDaysMapper {
public:
    CalendarBusinessDaysInputs toInputs(
        const quantra::CalendarBusinessDaysRequest* req) const;

    flatbuffers::Offset<quantra::CalendarBusinessDaysResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CalendarBusinessDaysResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CALENDAR_BUSINESS_DAYS_MAPPER_H
