#ifndef QUANTRA_CALENDAR_HOLIDAYS_PRICER_H
#define QUANTRA_CALENDAR_HOLIDAYS_PRICER_H

/**
 * CalendarHolidaysPricer — pure QuantLib core for the
 * "list of holidays in [start, end]" utility endpoint.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * calendar_holidays_mapper.{h,cpp}. The pricer touches no market data; the
 * `reg` parameter is unused and the registry is default-constructed by
 * ProductEndpoint for utility endpoints.
 */

#include <vector>

#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

struct CalendarHolidaysTrade {
    QuantLib::Calendar calendar;
    QuantLib::Date startDate;
    QuantLib::Date endDate;
    /// When false, weekends are filtered out (legacy default).
    bool includeWeekends = false;
};

struct CalendarHolidaysInputs {
    CalendarHolidaysTrade trade;
    /// Carried through to the response so the mapper can echo the requested
    /// calendar enum verbatim without re-reading the FlatBuffers request.
    quantra::enums::Calendar calendar = quantra::enums::Calendar_TARGET;
};

struct CalendarHolidaysResult {
    quantra::enums::Calendar calendar = quantra::enums::Calendar_TARGET;
    QuantLib::Date startDate;
    QuantLib::Date endDate;
    std::vector<QuantLib::Date> dates;
};

class CalendarHolidaysPricer {
public:
    CalendarHolidaysResult price(const CalendarHolidaysInputs& inputs,
                                 const PricingRegistry& reg,
                                 const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CALENDAR_HOLIDAYS_PRICER_H
