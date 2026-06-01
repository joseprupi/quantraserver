#ifndef QUANTRA_CALENDAR_ADVANCE_MAPPER_H
#define QUANTRA_CALENDAR_ADVANCE_MAPPER_H

/**
 * CalendarAdvanceMapper — the one place this product's flatbuffers live.
 * Decodes a CalendarAdvanceRequest into plain inputs and serializes the
 * pricer result back into a CalendarAdvanceResponse.
 */

#include "flatbuffers/grpc.h"

#include "calendar_advance_evaluator.h"
#include "calendar_advance_request_generated.h"
#include "calendar_advance_response_generated.h"

namespace quantra {

class CalendarAdvanceMapper {
public:
    CalendarAdvanceInputs toInputs(
        const quantra::CalendarAdvanceRequest* req) const;

    flatbuffers::Offset<quantra::CalendarAdvanceResponse> toResponse(
        flatbuffers::grpc::MessageBuilder& builder,
        const CalendarAdvanceResult& result) const;
};

} // namespace quantra

#endif // QUANTRA_CALENDAR_ADVANCE_MAPPER_H
