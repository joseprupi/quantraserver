#ifndef QUANTRA_CALENDAR_ADVANCE_PRICER_H
#define QUANTRA_CALENDAR_ADVANCE_PRICER_H

/**
 * CalendarAdvancePricer — pure QuantLib core for the
 * "advance a date by N units under a calendar/convention" utility endpoint.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * calendar_advance_mapper.{h,cpp}. The pricer touches no market data; the
 * `reg` parameter is unused and the registry is default-constructed by
 * ProductEndpoint for utility endpoints.
 */

#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>
#include <ql/time/period.hpp>

#include "pricing_context.h"
#include "pricing_registry.h"

namespace quantra {

struct CalendarAdvanceTrade {
    QuantLib::Calendar calendar;
    QuantLib::Date inputDate;
    QuantLib::Period period;
    QuantLib::BusinessDayConvention convention = QuantLib::Following;
    bool endOfMonth = false;
};

struct CalendarAdvanceInputs {
    CalendarAdvanceTrade trade;
    /// Carried through to the response so the mapper can echo the requested
    /// calendar enum verbatim without re-reading the FlatBuffers request.
    quantra::enums::Calendar calendar = quantra::enums::Calendar_TARGET;
};

struct CalendarAdvanceResult {
    quantra::enums::Calendar calendar = quantra::enums::Calendar_TARGET;
    QuantLib::Date inputDate;
    QuantLib::Date advancedDate;
};

class CalendarAdvancePricer {
public:
    CalendarAdvanceResult price(const CalendarAdvanceInputs& inputs,
                                const PricingRegistry& reg,
                                const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_CALENDAR_ADVANCE_PRICER_H
