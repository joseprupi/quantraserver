#ifndef QUANTRA_COMMODITIES_FUTURES_CONTRACT_H
#define QUANTRA_COMMODITIES_FUTURES_CONTRACT_H

/**
 * A futures contract: its delivery months in ascending order, each with its
 * last trading date and today's price, plus past daily settlements.
 *
 * Contract selection on a pricing day d, for a leg reading the `nearby`-th
 * contract (1 = front month) and rolling `rollDaysBeforeExpiry` = r pricing
 * days before expiry:
 *   roll date of a month = its last trading date moved back r pricing days
 *     (pricing days of this contract, own holidays included); with r = 0 it
 *     is the last trading date itself, so the expiring month is still read on
 *     its last trading date;
 *   front month = the first month whose roll date is on or after d;
 *   the month read = the front month moved `nearby` - 1 places down the list.
 * Example: last trading date Friday 20 November, r = 2: the roll date is
 * Wednesday 18 November; 19 and 20 November read the next month.
 */

#include "commodity_pricing_days.h"

#include <ql/time/date.hpp>

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace quantra { namespace commodities {

struct ContractMonth {
    /// Delivery month, "YYYY-MM".
    std::string month;
    /// Last trading date of this month.
    QuantLib::Date lastTradeDate;
    /// Today's futures price; needed only when the month is read after the
    /// valuation date (or on it without a fixing).
    std::optional<double> price;
};

struct ContractFixing {
    /// Delivery month the settlement belongs to, "YYYY-MM".
    std::string month;
    /// Settlement date.
    QuantLib::Date date;
    /// Settlement price.
    double value = 0.0;
};

class FuturesContract {
public:
    /// Validates the values: at least one month; every month "YYYY-MM"
    /// (IsYearMonth); months strictly ascending with strictly ascending last
    /// trading dates; prices and fixing values finite; fixing months
    /// "YYYY-MM"; no (month, date) given twice. Presence (id, dates) is the
    /// caller's: the request decoder checks it.
    FuturesContract(
        std::string id,
        PricingCalendar pricingCalendar,
        std::vector<ContractMonth> months,
        const std::vector<ContractFixing>& fixings);

    const std::string& id() const { return id_; }
    const PricingCalendar& pricingCalendar() const { return pricingCalendar_; }
    const std::vector<ContractMonth>& months() const { return months_; }

    /// Roll date of every month for a roll of `rollDaysBeforeExpiry` (>= 0)
    /// pricing days, in month order.
    std::vector<QuantLib::Date> rollDates(int rollDaysBeforeExpiry) const;

    /// Index into months() of the month read on `day` for `nearby` (>= 1;
    /// a leg checks it once with ValidateNearbyAndRoll). `rollDates` comes
    /// from rollDates(). Invalid argument naming the day when the list has no
    /// such month.
    std::size_t selectMonth(
        const QuantLib::Date& day, int nearby, const std::vector<QuantLib::Date>& rollDates) const;

    /// The settlement of `month` on `date`, when given.
    std::optional<double> fixing(const std::string& month, const QuantLib::Date& date) const;

private:
    std::string id_;
    PricingCalendar pricingCalendar_;
    std::vector<ContractMonth> months_;
    std::map<std::pair<std::string, QuantLib::Date>, double> fixings_;
};

/// nearby >= 1 and rollDaysBeforeExpiry >= 0; otherwise an invalid argument
/// naming `path.nearby` or `path.roll_days_before_expiry` (`path` is the
/// floating price, e.g. `leg2.price`).
void ValidateNearbyAndRoll(int nearby, int rollDaysBeforeExpiry, const std::string& path);

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_FUTURES_CONTRACT_H
