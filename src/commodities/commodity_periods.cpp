#include "commodity_periods.h"

#include "date_convert.h"
#include "error.h"

#include <algorithm>
#include <sstream>

namespace quantra { namespace commodities {

namespace {

/// Months per generated period; 0 for Once.
int monthsPerPeriod(QuantLib::Frequency frequency, const std::string& path) {
    switch (frequency) {
        case QuantLib::Monthly: return 1;
        case QuantLib::Quarterly: return 3;
        case QuantLib::Semiannual: return 6;
        case QuantLib::Annual: return 12;
        case QuantLib::Once: return 0;
        default: break;
    }
    std::ostringstream msg;
    msg << path << ".frequency " << frequency
        << " is not supported (Monthly, Quarterly, Semiannual, Annual or Once)";
    QUANTRA_INVALID_ARGUMENT(msg.str());
    return 0;
}

/// First day of the calendar-aligned period that follows the one holding
/// `day`: periods of `months` months start in January and every `months`
/// months after it.
QuantLib::Date nextBoundary(const QuantLib::Date& day, int months) {
    const int monthIndex = static_cast<int>(day.month()) - 1; // 0..11
    const int startOfBlock = monthIndex - monthIndex % months;
    int nextMonth = startOfBlock + months; // 1..12 months after January
    int year = day.year();
    while (nextMonth >= 12) {
        nextMonth -= 12;
        ++year;
    }
    return QuantLib::Date(1, static_cast<QuantLib::Month>(nextMonth + 1), year);
}

} // namespace

std::vector<CommodityPeriod> GeneratePeriods(
    const QuantLib::Date& start,
    const QuantLib::Date& end,
    QuantLib::Frequency frequency,
    const std::string& path) {
    const int months = monthsPerPeriod(frequency, path);
    if (start == QuantLib::Date() || end == QuantLib::Date()) {
        QUANTRA_INVALID_ARGUMENT(path + ": start_date and end_date are required");
    }
    if (start > end) {
        QUANTRA_INVALID_ARGUMENT(
            path + ".start_date " + DateToIso(start) + " is after end_date " + DateToIso(end));
    }
    std::vector<CommodityPeriod> out;
    if (months == 0) {
        out.push_back({start, end});
        return out;
    }
    QuantLib::Date current = start;
    while (current <= end) {
        const QuantLib::Date next = nextBoundary(current, months);
        const QuantLib::Date last = std::min(next - 1, end);
        out.push_back({current, last});
        current = next;
    }
    return out;
}

void ValidatePeriods(const std::vector<CommodityPeriod>& periods, const std::string& path) {
    if (periods.empty()) {
        QUANTRA_INVALID_ARGUMENT(path + ": at least one period is required");
    }
    for (std::size_t i = 0; i < periods.size(); ++i) {
        const auto& p = periods[i];
        const std::string at = path + "[" + std::to_string(i) + "]";
        if (p.start == QuantLib::Date() || p.end == QuantLib::Date()) {
            QUANTRA_INVALID_ARGUMENT(at + " needs start_date and end_date");
        }
        if (p.start > p.end) {
            QUANTRA_INVALID_ARGUMENT(
                at + ".start_date " + DateToIso(p.start) + " is after end_date " +
                DateToIso(p.end));
        }
        if (i > 0 && p.start <= periods[i - 1].end) {
            QUANTRA_INVALID_ARGUMENT(
                at + ".start_date " + DateToIso(p.start) + " is not after the end of " + path +
                "[" + std::to_string(i - 1) + "] (" + DateToIso(periods[i - 1].end) +
                "); periods must be ascending and must not overlap");
        }
    }
}

}} // namespace quantra::commodities
