#include "calendar_holidays_pricer.h"

#include "error.h"

namespace quantra {

CalendarHolidaysResult CalendarHolidaysPricer::price(
    const CalendarHolidaysInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    (void)reg;
    (void)ctx;

    const auto& trade = inputs.trade;
    if (trade.startDate > trade.endDate) {
        QUANTRA_ERROR(
            "CalendarHolidaysRequest.start_date must be <= end_date");
    }

    CalendarHolidaysResult result;
    result.calendar = inputs.calendar;
    result.startDate = trade.startDate;
    result.endDate = trade.endDate;

    for (QuantLib::Date d = trade.startDate; d <= trade.endDate; ++d) {
        if (trade.calendar.isBusinessDay(d)) {
            continue;
        }
        const bool isWeekend = trade.calendar.isWeekend(d.weekday());
        if (!trade.includeWeekends && isWeekend) {
            continue;
        }
        result.dates.push_back(d);
    }
    return result;
}

} // namespace quantra
