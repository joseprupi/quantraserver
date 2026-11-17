#include "commodity_futures_contract.h"

#include "date_convert.h"
#include "error.h"

#include <cmath>
#include <string>

namespace quantra { namespace commodities {

void ValidateNearbyAndRoll(int nearby, int rollDaysBeforeExpiry, const std::string& path) {
    if (nearby < 1) {
        QUANTRA_INVALID_ARGUMENT(
            path + ".nearby must be >= 1 (1 = front month), got " + std::to_string(nearby));
    }
    if (rollDaysBeforeExpiry < 0) {
        QUANTRA_INVALID_ARGUMENT(
            path + ".roll_days_before_expiry must be >= 0, got " +
            std::to_string(rollDaysBeforeExpiry));
    }
}

FuturesContract::FuturesContract(
    std::string id,
    PricingCalendar pricingCalendar,
    std::vector<ContractMonth> months,
    const std::vector<ContractFixing>& fixings)
    : id_(std::move(id)), pricingCalendar_(std::move(pricingCalendar)), months_(std::move(months)) {
    const std::string where = "commodity contract '" + id_ + "'";
    if (months_.empty()) {
        QUANTRA_INVALID_ARGUMENT(where + ": at least one month is required");
    }
    for (std::size_t i = 0; i < months_.size(); ++i) {
        const auto& m = months_[i];
        const std::string at = where + " months[" + std::to_string(i) + "]";
        if (!IsYearMonth(m.month)) {
            QUANTRA_INVALID_ARGUMENT(at + ": month '" + m.month + "' is not YYYY-MM");
        }
        if (m.price && !std::isfinite(*m.price)) {
            QUANTRA_INVALID_ARGUMENT(at + ": price must be finite");
        }
        if (i > 0) {
            const auto& prev = months_[i - 1];
            if (m.month <= prev.month) {
                QUANTRA_INVALID_ARGUMENT(
                    at + ": month " + m.month + " is not after " + prev.month +
                    "; months must be in ascending order");
            }
            if (m.lastTradeDate <= prev.lastTradeDate) {
                QUANTRA_INVALID_ARGUMENT(
                    at + ": last_trade_date " + DateToIso(m.lastTradeDate) + " of " + m.month +
                    " is not after " + DateToIso(prev.lastTradeDate) + " of " + prev.month);
            }
        }
    }
    for (std::size_t i = 0; i < fixings.size(); ++i) {
        const auto& f = fixings[i];
        const std::string at = where + " fixings[" + std::to_string(i) + "]";
        // A settlement of a month not in the list is accepted and never read.
        if (!IsYearMonth(f.month)) {
            QUANTRA_INVALID_ARGUMENT(at + ": month '" + f.month + "' is not YYYY-MM");
        }
        if (!std::isfinite(f.value)) {
            QUANTRA_INVALID_ARGUMENT(at + ": value must be finite");
        }
        if (!fixings_.emplace(std::make_pair(f.month, f.date), f.value).second) {
            QUANTRA_INVALID_ARGUMENT(
                at + ": a second fixing of " + f.month + " on " + DateToIso(f.date));
        }
    }
}

std::vector<QuantLib::Date> FuturesContract::rollDates(int rollDaysBeforeExpiry) const {
    std::vector<QuantLib::Date> out;
    out.reserve(months_.size());
    for (const auto& m : months_) {
        out.push_back(pricingCalendar_.pricingDaysBefore(m.lastTradeDate, rollDaysBeforeExpiry));
    }
    return out;
}

std::size_t FuturesContract::selectMonth(
    const QuantLib::Date& day, int nearby, const std::vector<QuantLib::Date>& rollDates) const {
    if (nearby < 1) {
        // Precondition (ValidateNearbyAndRoll names the field); guarded so a
        // smaller value never selects a month.
        QUANTRA_INVALID_ARGUMENT(
            "commodity contract '" + id_ + "': nearby must be >= 1, got " + std::to_string(nearby));
    }
    std::size_t front = 0;
    while (front < rollDates.size() && rollDates[front] < day) {
        ++front;
    }
    const std::size_t index = front + static_cast<std::size_t>(nearby - 1);
    if (index >= months_.size()) {
        QUANTRA_INVALID_ARGUMENT(
            "commodity contract '" + id_ + "': pricing day " + DateToIso(day) + " needs nearby " +
            std::to_string(nearby) + " after the last listed month " + months_.back().month);
    }
    return index;
}

std::optional<double> FuturesContract::fixing(
    const std::string& month, const QuantLib::Date& date) const {
    const auto it = fixings_.find(std::make_pair(month, date));
    if (it == fixings_.end()) {
        return std::nullopt;
    }
    return it->second;
}

}} // namespace quantra::commodities
