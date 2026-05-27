#include "calendar_holidays_request.h"

#include <sstream>
#include <vector>

#include <ql/quantlib.hpp>

#include "common.h"
#include "enums.h"
#include "error.h"

using namespace QuantLib;

namespace {

std::string dateToIsoString(const Date& d) {
    std::ostringstream os;
    os << DateToIso(d);
    return os.str();
}

} // namespace

flatbuffers::Offset<quantra::CalendarHolidaysResponse> CalendarHolidaysRequestHandler::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const quantra::CalendarHolidaysRequest* request) const {
    if (!request || !request->start_date() || !request->end_date()) {
        QUANTRA_ERROR("CalendarHolidaysRequest.start_date and end_date are required");
    }

    Date startDate = DateToQL(request->start_date()->str());
    Date endDate = DateToQL(request->end_date()->str());
    if (startDate > endDate) {
        QUANTRA_ERROR("CalendarHolidaysRequest.start_date must be <= end_date");
    }

    const Calendar calendar = CalendarToQL(request->calendar());
    const bool includeWeekends = request->include_weekends();

    std::vector<flatbuffers::Offset<flatbuffers::String>> dates;
    for (Date d = startDate; d <= endDate; ++d) {
        if (calendar.isBusinessDay(d)) {
            continue;
        }
        const bool isWeekend = calendar.isWeekend(d.weekday());
        if (!includeWeekends && isWeekend) {
            continue;
        }
        dates.push_back(builder->CreateString(dateToIsoString(d)));
    }

    auto startDateStr = builder->CreateString(dateToIsoString(startDate));
    auto endDateStr = builder->CreateString(dateToIsoString(endDate));
    auto dateVec = builder->CreateVector(dates);

    quantra::CalendarHolidaysResponseBuilder responseBuilder(*builder);
    responseBuilder.add_calendar(request->calendar());
    responseBuilder.add_start_date(startDateStr);
    responseBuilder.add_end_date(endDateStr);
    responseBuilder.add_dates(dateVec);
    responseBuilder.add_count(static_cast<uint32_t>(dates.size()));
    return responseBuilder.Finish();
}
