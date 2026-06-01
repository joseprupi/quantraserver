#include "calendar_business_days_mapper.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

CalendarBusinessDaysInputs CalendarBusinessDaysMapper::toInputs(
    const quantra::CalendarBusinessDaysRequest* req) const {
    if (req == nullptr) {
        QUANTRA_INVALID_ARGUMENT("CalendarBusinessDaysRequest is null");
    }
    if (!req->start_date()) {
        QUANTRA_INVALID_ARGUMENT(
            "CalendarBusinessDaysRequest.start_date is required");
    }
    if (!req->end_date()) {
        QUANTRA_INVALID_ARGUMENT(
            "CalendarBusinessDaysRequest.end_date is required");
    }

    CalendarBusinessDaysInputs inputs;
    inputs.calendar = req->calendar();
    inputs.trade.calendar = CalendarToQL(req->calendar());
    inputs.trade.startDate = DateToQL(req->start_date()->str());
    inputs.trade.endDate = DateToQL(req->end_date()->str());
    inputs.trade.includeStart = req->include_start();
    inputs.trade.includeEnd = req->include_end();
    return inputs;
}

flatbuffers::Offset<quantra::CalendarBusinessDaysResponse>
CalendarBusinessDaysMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const CalendarBusinessDaysResult& result) const {

    std::vector<flatbuffers::Offset<flatbuffers::String>> dates;
    dates.reserve(result.dates.size());
    for (const auto& d : result.dates) {
        dates.push_back(builder.CreateString(DateToIso(d)));
    }

    auto startDateStr = builder.CreateString(DateToIso(result.startDate));
    auto endDateStr = builder.CreateString(DateToIso(result.endDate));
    auto dateVec = builder.CreateVector(dates);

    quantra::CalendarBusinessDaysResponseBuilder rb(builder);
    rb.add_calendar(result.calendar);
    rb.add_start_date(startDateStr);
    rb.add_end_date(endDateStr);
    rb.add_dates(dateVec);
    rb.add_count(static_cast<uint32_t>(result.dates.size()));
    return rb.Finish();
}

} // namespace quantra
