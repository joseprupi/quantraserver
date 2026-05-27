#include "calendar_business_days_request.h"

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

flatbuffers::Offset<quantra::CalendarBusinessDaysResponse> CalendarBusinessDaysRequestHandler::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const quantra::CalendarBusinessDaysRequest* request) const {
    if (!request || !request->start_date() || !request->end_date()) {
        QUANTRA_ERROR("CalendarBusinessDaysRequest.start_date and end_date are required");
    }

    Date startDate = DateToQL(request->start_date()->str());
    Date endDate = DateToQL(request->end_date()->str());
    if (startDate > endDate) {
        QUANTRA_ERROR("CalendarBusinessDaysRequest.start_date must be <= end_date");
    }

    const Calendar calendar = CalendarToQL(request->calendar());
    const bool includeStart = request->include_start();
    const bool includeEnd = request->include_end();

    std::vector<flatbuffers::Offset<flatbuffers::String>> dates;
    for (Date d = startDate; d <= endDate; ++d) {
        if (!includeStart && d == startDate) {
            continue;
        }
        if (!includeEnd && d == endDate) {
            continue;
        }
        if (calendar.isBusinessDay(d)) {
            dates.push_back(builder->CreateString(dateToIsoString(d)));
        }
    }

    auto startDateStr = builder->CreateString(dateToIsoString(startDate));
    auto endDateStr = builder->CreateString(dateToIsoString(endDate));
    auto dateVec = builder->CreateVector(dates);

    quantra::CalendarBusinessDaysResponseBuilder responseBuilder(*builder);
    responseBuilder.add_calendar(request->calendar());
    responseBuilder.add_start_date(startDateStr);
    responseBuilder.add_end_date(endDateStr);
    responseBuilder.add_dates(dateVec);
    responseBuilder.add_count(static_cast<uint32_t>(dates.size()));
    return responseBuilder.Finish();
}
