#include "commodity_pricing_days.h"

#include "date_convert.h"
#include "error.h"

namespace quantra { namespace commodities {

PricingCalendar::PricingCalendar(
    QuantLib::Calendar calendar, const std::vector<QuantLib::Date>& ownHolidays)
    : calendar_(std::move(calendar)), ownHolidays_(ownHolidays.begin(), ownHolidays.end()) {
    if (calendar_.empty()) {
        QUANTRA_INVALID_ARGUMENT("commodity pricing calendar is required");
    }
}

bool PricingCalendar::isPricingDay(const QuantLib::Date& day) const {
    return calendar_.isBusinessDay(day) && ownHolidays_.count(day) == 0;
}

std::vector<QuantLib::Date> PricingCalendar::pricingDays(
    const QuantLib::Date& start, const QuantLib::Date& end) const {
    std::vector<QuantLib::Date> out;
    for (QuantLib::Date d = start; d <= end; ++d) {
        if (isPricingDay(d)) {
            out.push_back(d);
        }
    }
    return out;
}

QuantLib::Date PricingCalendar::pricingDaysBefore(const QuantLib::Date& day, int n) const {
    if (n < 0) {
        QUANTRA_INVALID_ARGUMENT("commodity pricing calendar: a negative number of pricing days");
    }
    // Ten years without a pricing day means a calendar that never prices.
    const QuantLib::Date floor = day - 3660;
    QuantLib::Date d = day;
    while (n > 0) {
        --d;
        if (d < floor) {
            QUANTRA_INVALID_ARGUMENT(
                "commodity pricing calendar: no pricing day found before " + DateToIso(day));
        }
        if (isPricingDay(d)) {
            --n;
        }
    }
    return d;
}

std::vector<QuantLib::Date> PeriodPricingDays(
    const PricingCalendar& calendar, const CommodityPeriod& period, const std::string& path) {
    auto days = calendar.pricingDays(period.start, period.end);
    if (days.empty()) {
        CommodityReadData data;
        data.reason = "NoPricingDay";
        data.path = path;
        throw CommodityReadError(
            path + " (" + DateToIso(period.start) + " to " + DateToIso(period.end) +
                ") has no pricing day",
            data);
    }
    return days;
}

}} // namespace quantra::commodities
