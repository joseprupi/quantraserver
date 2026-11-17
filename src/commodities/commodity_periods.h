#ifndef QUANTRA_COMMODITIES_PERIODS_H
#define QUANTRA_COMMODITIES_PERIODS_H

/**
 * Calculation periods of a commodity swap leg.
 *
 * A period owns the pricing days inside it: both its first and its last day
 * are included, and its dates are never adjusted for weekends or holidays
 * (the pricing days are read inside it from the pricing calendar).
 *
 * Generated periods are calendar months, quarters (January, April, July,
 * October), half-years (January, July) or years, the first one cut to start
 * at the range's first day and the last one cut to end at its last day:
 * 2026-11-15 to 2027-03-31 Monthly gives 15-30 November, December, January,
 * February and March. `Once` gives one period covering the range.
 */

#include <ql/time/date.hpp>
#include <ql/time/frequency.hpp>

#include <string>
#include <vector>

namespace quantra { namespace commodities {

struct CommodityPeriod {
    /// First day of the period, included.
    QuantLib::Date start;
    /// Last day of the period, included.
    QuantLib::Date end;
};

/// Periods from `start` to `end` (both included) at `frequency`: Monthly,
/// Quarterly, Semiannual, Annual or Once. Any other frequency, or
/// start > end, is an invalid argument naming `path` (the generated form's
/// field, e.g. `leg1.periods`) and its `start_date` / `end_date` /
/// `frequency`.
std::vector<CommodityPeriod> GeneratePeriods(
    const QuantLib::Date& start,
    const QuantLib::Date& end,
    QuantLib::Frequency frequency,
    const std::string& path);

/// Periods of a leg: at least one, each with start <= end, in ascending
/// order, no two overlapping (gaps are allowed). Invalid argument otherwise,
/// naming `path` (the period list, e.g. `leg1.periods.periods`) and the
/// period's index in it.
void ValidatePeriods(const std::vector<CommodityPeriod>& periods, const std::string& path);

}} // namespace quantra::commodities

#endif // QUANTRA_COMMODITIES_PERIODS_H
