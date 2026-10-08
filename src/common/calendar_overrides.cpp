#include "calendar_overrides.h"

#include "date_convert.h"
#include "error.h"

#include <ql/time/calendars/nullcalendar.hpp>

#include <algorithm>
#include <map>
#include <set>

namespace quantra {

namespace {

// Process-level record of what is currently applied. One request at a time per
// worker, so no synchronisation is needed (same model as the evaluation date).
std::vector<QuantLib::Calendar> g_touched;
std::string g_fingerprint;

std::string at(const std::string& path, std::size_t i) {
    return path + "[" + std::to_string(i) + "]";
}

std::string buildFingerprint(const std::vector<HolidayOverride>& overrides) {
    // Calendar names are unique within a validated set.
    std::map<std::string, std::string> byName;
    for (const auto& o : overrides) {
        if (o.added.empty() && o.removed.empty()) continue;
        std::vector<std::string> dates;
        for (const auto& d : o.added)
            dates.push_back("+" + DateToIso(d));
        for (const auto& d : o.removed)
            dates.push_back("-" + DateToIso(d));
        // Order by date, then sign; a date is in at most one list.
        std::sort(dates.begin(), dates.end(), [](const std::string& a, const std::string& b) {
            const int c = a.compare(1, std::string::npos, b, 1, std::string::npos);
            return c != 0 ? c < 0 : a[0] < b[0];
        });
        std::string body;
        for (const auto& d : dates) {
            if (!body.empty()) body += ",";
            body += d;
        }
        byName[o.calendar.name()] = body;
    }
    std::string out;
    for (const auto& kv : byName) {
        if (!out.empty()) out += ";";
        out += kv.first + "[" + kv.second + "]";
    }
    return out;
}

} // namespace

bool calendarSupportsOverrides(const QuantLib::Calendar& calendar) {
    if (calendar.empty()) return false;
    const std::string name = calendar.name();
    // BespokeCalendar (unnamed, as built from the wire enum) and NullCalendar
    // allocate a private implementation per instance.
    return !name.empty() && name != QuantLib::NullCalendar().name();
}

void validateCalendarOverrides(
    const std::vector<HolidayOverride>& overrides, const std::string& path) {
    std::map<std::string, std::size_t> seenCalendars;
    for (std::size_t i = 0; i < overrides.size(); ++i) {
        const auto& o = overrides[i];
        const std::string entry = at(path, i);
        if (!calendarSupportsOverrides(o.calendar)) {
            QUANTRA_INVALID_ARGUMENT(
                entry + ".calendar: holiday overrides are not supported for this "
                        "calendar (BespokeCalendar and NullCalendar cannot be overridden)");
        }
        const auto dup = seenCalendars.emplace(o.calendar.name(), i);
        if (!dup.second) {
            QUANTRA_INVALID_ARGUMENT(
                entry + ".calendar resolves to the same calendar ('" + o.calendar.name() +
                "') as " + at(path, dup.first->second) + "; list each calendar once");
        }
        std::set<QuantLib::Date> added;
        for (std::size_t j = 0; j < o.added.size(); ++j) {
            if (!added.insert(o.added[j]).second) {
                QUANTRA_INVALID_ARGUMENT(
                    at(entry + ".added_holidays", j) + ": duplicate date " + DateToIso(o.added[j]));
            }
        }
        std::set<QuantLib::Date> removed;
        for (std::size_t j = 0; j < o.removed.size(); ++j) {
            const auto& d = o.removed[j];
            const std::string field = at(entry + ".removed_holidays", j);
            if (!removed.insert(d).second) {
                QUANTRA_INVALID_ARGUMENT(field + ": duplicate date " + DateToIso(d));
            }
            if (added.count(d) != 0) {
                QUANTRA_INVALID_ARGUMENT(
                    field + ": " + DateToIso(d) +
                    " is also listed in added_holidays of the same entry");
            }
            if (o.calendar.isWeekend(d.weekday())) {
                QUANTRA_INVALID_ARGUMENT(
                    field + ": " + DateToIso(d) +
                    " is a weekend day; a weekend cannot be turned into a business day");
            }
        }
    }
}

void applyCalendarOverrides(
    const std::vector<HolidayOverride>& overrides, const std::string& path) {
    validateCalendarOverrides(overrides, path);
    resetCalendarOverrides();
    for (const auto& o : overrides) {
        // Record before mutating so a reset always covers a partial apply.
        g_touched.push_back(o.calendar);
        QuantLib::Calendar calendar = o.calendar;
        for (const auto& d : o.added)
            calendar.addHoliday(d);
        for (const auto& d : o.removed)
            calendar.removeHoliday(d);
    }
    g_fingerprint = buildFingerprint(overrides);
}

void resetCalendarOverrides() {
    for (auto& calendar : g_touched)
        calendar.resetAddedAndRemovedHolidays();
    g_touched.clear();
    g_fingerprint.clear();
}

const std::string& activeCalendarOverridesFingerprint() {
    return g_fingerprint;
}

} // namespace quantra
