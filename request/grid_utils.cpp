#include "grid_utils.h"

#include <algorithm>
#include <sstream>

namespace quantra {
namespace grid_utils {

std::string ToIsoDate(const QuantLib::Date& d) {
    std::ostringstream os;
    os << QuantLib::io::iso_date(d);
    return os.str();
}

QuantLib::Calendar ResolveCalendar(
    const DateGridSpec* gridSpec,
    const QueryOptions* options,
    const QuantLib::Calendar& fallbackCalendar) {
    if (options && options->calendar() != enums::Calendar_NullCalendar) {
        return CalendarToQL(options->calendar());
    }
    if (!gridSpec) {
        return fallbackCalendar;
    }
    if (gridSpec->grid_type() == DateGrid_TenorGrid) {
        auto grid = gridSpec->grid_as_TenorGrid();
        if (grid && grid->calendar() != enums::Calendar_NullCalendar) {
            return CalendarToQL(grid->calendar());
        }
    } else if (gridSpec->grid_type() == DateGrid_RangeGrid) {
        auto grid = gridSpec->grid_as_RangeGrid();
        if (grid && grid->calendar() != enums::Calendar_NullCalendar) {
            return CalendarToQL(grid->calendar());
        }
    }
    return fallbackCalendar;
}

QuantLib::BusinessDayConvention ResolveBusinessDayConvention(
    const DateGridSpec* gridSpec,
    const QueryOptions* options,
    QuantLib::BusinessDayConvention fallbackBdc) {
    if (options) {
        return ConventionToQL(options->business_day_convention());
    }
    if (!gridSpec) {
        return fallbackBdc;
    }
    if (gridSpec->grid_type() == DateGrid_TenorGrid) {
        auto grid = gridSpec->grid_as_TenorGrid();
        return grid ? ConventionToQL(grid->business_day_convention()) : fallbackBdc;
    }
    if (gridSpec->grid_type() == DateGrid_RangeGrid) {
        auto grid = gridSpec->grid_as_RangeGrid();
        return grid ? ConventionToQL(grid->business_day_convention()) : fallbackBdc;
    }
    return fallbackBdc;
}

std::vector<QuantLib::Date> BuildTenorGrid(
    const TenorGrid* grid,
    const QuantLib::Date& referenceDate,
    const QuantLib::Calendar& fallbackCalendar,
    bool forceCalendarAdvance) {
    if (!grid || !grid->tenors() || grid->tenors()->size() == 0) {
        QUANTRA_ERROR("TenorGrid.tenors is required");
    }

    std::vector<QuantLib::Date> dates;
    dates.reserve(grid->tenors()->size());

    QuantLib::Calendar calendar = fallbackCalendar;
    QuantLib::BusinessDayConvention bdc = QuantLib::Following;
    bool useCalendar = forceCalendarAdvance;
    if (grid->calendar() != enums::Calendar_NullCalendar) {
        calendar = CalendarToQL(grid->calendar());
        bdc = ConventionToQL(grid->business_day_convention());
        useCalendar = true;
    }

    for (flatbuffers::uoffset_t i = 0; i < grid->tenors()->size(); ++i) {
        auto tenor = grid->tenors()->Get(i);
        if (!tenor) {
            QUANTRA_ERROR("TenorGrid.tenors[] contains null");
        }
        QuantLib::Period p(tenor->n(), TimeUnitToQL(tenor->unit()));
        if (p.length() == 0 && p.units() != QuantLib::Days) {
            QUANTRA_ERROR("TenorGrid only allows zero period as 0 Days");
        }
        QuantLib::Date d = useCalendar
            ? calendar.advance(referenceDate, p, bdc)
            : referenceDate + p;
        dates.push_back(d);
    }

    return dates;
}

std::vector<QuantLib::Date> BuildRangeGrid(
    const RangeGrid* grid,
    const QuantLib::Date& asOfDate,
    int maxPoints) {
    if (!grid || !grid->end_date()) {
        QUANTRA_ERROR("RangeGrid.end_date is required");
    }

    QuantLib::Date startDate = grid->start_date() ? DateToQL(grid->start_date()->str()) : asOfDate;
    QuantLib::Date endDate = DateToQL(grid->end_date()->str());
    int stepNumber = std::max(1, grid->step_number());
    QuantLib::TimeUnit stepUnit = TimeUnitToQL(grid->step_time_unit());
    QuantLib::Period step(stepNumber, stepUnit);
    bool businessDaysOnly = grid->business_days_only();
    QuantLib::Calendar calendar = CalendarToQL(grid->calendar());
    QuantLib::BusinessDayConvention bdc = ConventionToQL(grid->business_day_convention());

    if (businessDaysOnly && grid->calendar() == enums::Calendar_NullCalendar) {
        calendar = QuantLib::WeekendsOnly();
    }

    std::vector<QuantLib::Date> dates;
    QuantLib::Date current = startDate;
    while (current <= endDate) {
        if (!businessDaysOnly || calendar.isBusinessDay(current)) {
            dates.push_back(current);
        }
        if (static_cast<int>(dates.size()) > maxPoints) {
            QUANTRA_ERROR("Grid too large (>" + std::to_string(maxPoints) + " points).");
        }
        if (stepUnit == QuantLib::Days) {
            current = current + stepNumber;
        } else if (stepUnit == QuantLib::Weeks) {
            current = current + 7 * stepNumber;
        } else {
            current = calendar.advance(current, step, bdc);
        }
    }
    return dates;
}

} // namespace grid_utils
} // namespace quantra
