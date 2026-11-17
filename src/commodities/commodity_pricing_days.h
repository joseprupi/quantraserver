#ifndef QUANTRA_COMMODITIES_PRICING_DAYS_H
#define QUANTRA_COMMODITIES_PRICING_DAYS_H

/**
 * Pricing days: the days a futures contract settles or a spot price is
 * published. They are the business days of a named calendar minus the
 * contract's (or spot price's) own extra holidays. The extra holidays belong
 * to this object only and change no other use of the named calendar; this is
 * how an exchange's own holidays are given when the named calendar differs
 * from the exchange's.
 */

#include "commodity_periods.h"

#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>

#include <set>
#include <string>
#include <vector>

namespace quantra { namespace commodities {

class PricingCalendar {
public:
    /// `calendar` must not be empty (invalid argument otherwise).
    PricingCalendar(QuantLib::Calendar calendar, const std::vector<QuantLib::Date>& ownHolidays);

    /// A business day of the calendar that is not one of the own holidays.
    bool isPricingDay(const QuantLib::Date& day) const;

    /// Pricing days in [start, end], both included, ascending (may be empty).
    std::vector<QuantLib::Date> pricingDays(
        const QuantLib::Date& start, const QuantLib::Date& end) const;

    /// The date `n` pricing days before `day` (n >= 0); `day` itself when
    /// n = 0, whether or not it is a pricing day.
    QuantLib::Date pricingDaysBefore(const QuantLib::Date& day, int n) const;

    const QuantLib::Calendar& calendar() const { return calendar_; }

private:
    QuantLib::Calendar calendar_;
    std::set<QuantLib::Date> ownHolidays_;
};

/// Pricing days of a floating period; invalid argument naming `path` (the
/// period, e.g. `leg2.periods[3]`) and its dates when there are none.
std::vector<QuantLib::Date> PeriodPricingDays(
    const PricingCalendar& calendar, const CommodityPeriod& period, const std::string& path);

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_PRICING_DAYS_H
