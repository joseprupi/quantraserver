#include "calendar_advance_mapper.h"

#include "common.h"
#include "enums.h"
#include "error.h"

namespace quantra {

CalendarAdvanceInputs CalendarAdvanceMapper::toInputs(
    const quantra::CalendarAdvanceRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("CalendarAdvanceRequest is null");
    }
    if (!req->date()) {
        QUANTRA_INVALID_ARGUMENT("CalendarAdvanceRequest.date is required");
    }

    CalendarAdvanceInputs inputs;
    inputs.calendar = req->calendar();
    inputs.trade.calendar = CalendarToQL(req->calendar());
    inputs.trade.inputDate = DateToQL(req->date()->str());
    inputs.trade.period =
        QuantLib::Period(req->tenor_number(), TimeUnitToQL(req->tenor_unit()));
    inputs.trade.convention = ConventionToQL(req->convention());
    inputs.trade.endOfMonth = req->end_of_month();
    return inputs;
}

flatbuffers::Offset<quantra::CalendarAdvanceResponse>
CalendarAdvanceMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const CalendarAdvanceResult& result) const {

    auto inputDateStr = builder.CreateString(DateToIso(result.inputDate));
    auto advancedDateStr = builder.CreateString(DateToIso(result.advancedDate));

    quantra::CalendarAdvanceResponseBuilder rb(builder);
    rb.add_calendar(result.calendar);
    rb.add_input_date(inputDateStr);
    rb.add_advanced_date(advancedDateStr);
    return rb.Finish();
}

} // namespace quantra
