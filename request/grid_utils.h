#ifndef QUANTRASERVER_GRID_UTILS_H
#define QUANTRASERVER_GRID_UTILS_H

#include <vector>
#include <string>

#include <ql/time/date.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/businessdayconvention.hpp>

#include "curve_query_generated.h"
#include "common.h"
#include "enums.h"
#include "error.h"

namespace quantra {
namespace grid_utils {

std::string ToIsoDate(const QuantLib::Date& d);

QuantLib::Calendar ResolveCalendar(
    const DateGridSpec* gridSpec,
    const QueryOptions* options,
    const QuantLib::Calendar& fallbackCalendar);

QuantLib::BusinessDayConvention ResolveBusinessDayConvention(
    const DateGridSpec* gridSpec,
    const QueryOptions* options,
    QuantLib::BusinessDayConvention fallbackBdc = QuantLib::Following);

std::vector<QuantLib::Date> BuildTenorGrid(
    const TenorGrid* grid,
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& fallbackCalendar,
    bool forceCalendarAdvance);

std::vector<QuantLib::Date> BuildRangeGrid(
    const RangeGrid* grid,
    const QuantLib::Date& asOfDate,
    int maxPoints);

} // namespace grid_utils
} // namespace quantra

#endif // QUANTRASERVER_GRID_UTILS_H
