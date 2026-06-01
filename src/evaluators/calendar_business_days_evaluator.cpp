#include "calendar_business_days_evaluator.h"

#include "error.h"

namespace quantra {

CalendarBusinessDaysResult CalendarBusinessDaysEvaluator::evaluate(
    const CalendarBusinessDaysInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    (void)reg;
    (void)ctx;

    const auto& trade = inputs.trade;
    if (trade.startDate > trade.endDate) {
        QUANTRA_INVALID_ARGUMENT(
            "CalendarBusinessDaysRequest.start_date must be <= end_date");
    }

    CalendarBusinessDaysResult result;
    result.calendar = inputs.calendar;
    result.startDate = trade.startDate;
    result.endDate = trade.endDate;

    for (QuantLib::Date d = trade.startDate; d <= trade.endDate; ++d) {
        if (!trade.includeStart && d == trade.startDate) {
            continue;
        }
        if (!trade.includeEnd && d == trade.endDate) {
            continue;
        }
        if (trade.calendar.isBusinessDay(d)) {
            result.dates.push_back(d);
        }
    }
    return result;
}

} // namespace quantra
