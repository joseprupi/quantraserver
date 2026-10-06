#ifndef QUANTRASERVER_CALENDAR_OVERRIDE_PARSER_H
#define QUANTRASERVER_CALENDAR_OVERRIDE_PARSER_H

/**
 * Calendar Override Parser
 *
 * Parses the FlatBuffers `calendar_overrides` vector (Pricing and the calendar
 * utility requests) into FlatBuffers-free HolidayOverride values. Field-level
 * checks (calendar present, dates parseable) happen here; the set-level rules
 * are enforced by applyCalendarOverrides.
 */

#include <string>
#include <vector>

#include "calendar_override_generated.h"
#include "calendar_overrides.h"

namespace quantra {

/// `path` is the field path used in error messages, e.g.
/// "pricing.calendar_overrides". A null vector yields an empty result.
std::vector<HolidayOverride> parseCalendarOverrides(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::CalendarOverride>>* overrides,
    const std::string& path);

/// Parse, validate and apply in one step (the request entry point).
void applyRequestCalendarOverrides(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::CalendarOverride>>* overrides,
    const std::string& path);

} // namespace quantra

#endif // QUANTRASERVER_CALENDAR_OVERRIDE_PARSER_H
