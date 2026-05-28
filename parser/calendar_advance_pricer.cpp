#include "calendar_advance_pricer.h"

namespace quantra {

CalendarAdvanceResult CalendarAdvancePricer::price(
    const CalendarAdvanceInputs& inputs,
    const PricingRegistry& reg,
    const PricingContext& ctx) const {
    (void)reg;
    (void)ctx;

    const auto& trade = inputs.trade;

    CalendarAdvanceResult result;
    result.calendar = inputs.calendar;
    result.inputDate = trade.inputDate;
    result.advancedDate = trade.calendar.advance(
        trade.inputDate, trade.period, trade.convention, trade.endOfMonth);
    return result;
}

} // namespace quantra
