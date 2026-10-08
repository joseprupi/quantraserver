#ifndef QUANTRASERVER_GRID_UTILS_H
#define QUANTRASERVER_GRID_UTILS_H

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"

#include "curve_query_generated.h"

#include <ql/time/businessdayconvention.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>

#include <string>
#include <vector>

namespace quantra { namespace grid_utils {

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
    const RangeGrid* grid, const QuantLib::Date& asOfDate, int maxPoints);

}} // namespace quantra::grid_utils

#endif // QUANTRASERVER_GRID_UTILS_H
