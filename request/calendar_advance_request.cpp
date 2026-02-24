#include "calendar_advance_request.h"

#include <sstream>

#include <ql/quantlib.hpp>

#include "common.h"
#include "enums.h"
#include "error.h"

using namespace QuantLib;

namespace {

std::string dateToIsoString(const Date& d) {
    std::ostringstream os;
    os << io::iso_date(d);
    return os.str();
}

} // namespace

flatbuffers::Offset<quantra::CalendarAdvanceResponse> CalendarAdvanceRequestHandler::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const quantra::CalendarAdvanceRequest* request) const {
    if (!request || !request->date()) {
        QUANTRA_ERROR("CalendarAdvanceRequest.date is required");
    }

    const Date inputDate = DateToQL(request->date()->str());
    const Calendar calendar = CalendarToQL(request->calendar());
    const BusinessDayConvention bdc = ConventionToQL(request->convention());
    const TimeUnit unit = TimeUnitToQL(request->tenor_unit());
    const Period period(request->tenor_number(), unit);

    const Date advanced = calendar.advance(inputDate, period, bdc, request->end_of_month());

    auto inputDateStr = builder->CreateString(dateToIsoString(inputDate));
    auto advancedDateStr = builder->CreateString(dateToIsoString(advanced));

    quantra::CalendarAdvanceResponseBuilder responseBuilder(*builder);
    responseBuilder.add_calendar(request->calendar());
    responseBuilder.add_input_date(inputDateStr);
    responseBuilder.add_advanced_date(advancedDateStr);
    return responseBuilder.Finish();
}
