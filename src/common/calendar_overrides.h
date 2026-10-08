#ifndef QUANTRA_CALENDAR_OVERRIDES_H
#define QUANTRA_CALENDAR_OVERRIDES_H

/**
 * Per-request calendar holiday overrides.
 *
 * A request may assert "this date is a holiday" / "this date is a business
 * day" for a calendar. QuantLib keeps added/removed holidays on the calendar's
 * shared implementation, i.e. process-global per calendar, so the overrides are
 * applied for the duration of one request and reset afterwards — the same
 * set/reset pattern as the evaluation date (EvalDateGuard). This is safe because
 * a worker processes one request at a time.
 *
 * This module is FlatBuffers-free; the wire -> HolidayOverride translation lives
 * in src/parsers/calendar_override_parser.
 */

#include <ql/time/calendar.hpp>
#include <ql/time/date.hpp>

#include <string>
#include <vector>

namespace quantra {

/// Holiday overrides for one calendar, as requested (not QuantLib's effective
/// sets: a date that is already a holiday / business day stays listed here).
struct HolidayOverride {
    QuantLib::Calendar calendar;
    std::vector<QuantLib::Date> added;   // must be holidays
    std::vector<QuantLib::Date> removed; // must be business days
};

/// False for calendars whose QuantLib instances each carry private state
/// (BespokeCalendar, NullCalendar) or that are empty: an override applied to
/// one instance would not be seen by the instances the pricing code builds.
bool calendarSupportsOverrides(const QuantLib::Calendar& calendar);

/// Validate a full override set; throws QuantraInvalidArgument naming the
/// offending field (`path[i].field[j]`) on the first violation:
/// unsupported calendar, the same calendar (by QuantLib name, so enum aliases
/// collapse) in two entries, a duplicate date inside one list, a date in both
/// lists of one entry, or a weekend date in `removed`.
void validateCalendarOverrides(
    const std::vector<HolidayOverride>& overrides, const std::string& path = "calendar_overrides");

/// Validate everything first, then apply to QuantLib's global calendar state
/// and record the touched calendars and the canonical fingerprint. Any
/// previously applied overrides are reset first.
void applyCalendarOverrides(
    const std::vector<HolidayOverride>& overrides, const std::string& path = "calendar_overrides");

/// Undo every override applied through applyCalendarOverrides and clear the
/// fingerprint. Safe to call when nothing is applied.
void resetCalendarOverrides();

/// Canonical description of the overrides currently in force: "" when none,
/// otherwise calendars sorted by name, each with its sorted ISO dates prefixed
/// `+` (added) / `-` (removed), e.g. "TARGET[+2024-06-14,-2024-05-01]".
/// Independent of entry order and of date order inside the request.
const std::string& activeCalendarOverridesFingerprint();

/// RAII guard: resets the overrides on entry (a request never depends on a
/// previous request's cleanup) and again on exit (an idle worker is clean).
struct CalendarOverridesGuard {
    CalendarOverridesGuard() { resetCalendarOverrides(); }
    ~CalendarOverridesGuard() { resetCalendarOverrides(); }
    CalendarOverridesGuard(const CalendarOverridesGuard&) = delete;
    CalendarOverridesGuard& operator=(const CalendarOverridesGuard&) = delete;
};

} // namespace quantra

#endif // QUANTRA_CALENDAR_OVERRIDES_H
