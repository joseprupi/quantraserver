#include "calendar_holidays_mapper.h"

#include "common.h"
#include "enums.h"
#include "error.h"

namespace quantra {

CalendarHolidaysInputs CalendarHolidaysMapper::toInputs(
    const quantra::CalendarHolidaysRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("CalendarHolidaysRequest is null");
    }
    if (!req->start_date()) {
        QUANTRA_INVALID_ARGUMENT(
            "CalendarHolidaysRequest.start_date is required");
    }
    if (!req->end_date()) {
        QUANTRA_INVALID_ARGUMENT(
            "CalendarHolidaysRequest.end_date is required");
    }

    CalendarHolidaysInputs inputs;
    inputs.calendar = req->calendar();
    inputs.trade.calendar = CalendarToQL(req->calendar());
    inputs.trade.startDate = DateToQL(req->start_date()->str());
    inputs.trade.endDate = DateToQL(req->end_date()->str());
    inputs.trade.includeWeekends = req->include_weekends();
    return inputs;
}

flatbuffers::Offset<quantra::CalendarHolidaysResponse>
CalendarHolidaysMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const CalendarHolidaysResult& result) const {

    std::vector<flatbuffers::Offset<flatbuffers::String>> dates;
    dates.reserve(result.dates.size());
    for (const auto& d : result.dates) {
        dates.push_back(builder.CreateString(DateToIso(d)));
    }

    auto startDateStr = builder.CreateString(DateToIso(result.startDate));
    auto endDateStr = builder.CreateString(DateToIso(result.endDate));
    auto dateVec = builder.CreateVector(dates);

    quantra::CalendarHolidaysResponseBuilder rb(builder);
    rb.add_calendar(result.calendar);
    rb.add_start_date(startDateStr);
    rb.add_end_date(endDateStr);
    rb.add_dates(dateVec);
    rb.add_count(static_cast<uint32_t>(result.dates.size()));
    return rb.Finish();
}

} // namespace quantra
