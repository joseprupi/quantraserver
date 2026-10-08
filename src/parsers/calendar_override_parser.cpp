#include "calendar_override_parser.h"

#include "date_convert.h"
#include "enum_convert.h"
#include "error.h"
#include "request_validation.h"

namespace quantra {

namespace {

std::vector<QuantLib::Date> parseDates(
    const flatbuffers::Vector<flatbuffers::Offset<flatbuffers::String>>* dates,
    const std::string& field) {
    std::vector<QuantLib::Date> out;
    if (dates == nullptr) return out;
    out.reserve(dates->size());
    for (flatbuffers::uoffset_t j = 0; j < dates->size(); ++j) {
        try {
            out.push_back(DateToQL(dates->Get(j)->str()));
        } catch (const std::exception& e) {
            QUANTRA_INVALID_ARGUMENT(field + "[" + std::to_string(j) + "]: " + e.what());
        }
    }
    return out;
}

} // namespace

std::vector<HolidayOverride> parseCalendarOverrides(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::CalendarOverride>>* overrides,
    const std::string& path) {
    std::vector<HolidayOverride> out;
    if (overrides == nullptr) return out;
    out.reserve(overrides->size());
    for (flatbuffers::uoffset_t i = 0; i < overrides->size(); ++i) {
        const auto* entry = overrides->Get(i);
        const std::string entryPath = path + "[" + std::to_string(i) + "]";
        HolidayOverride o;
        o.calendar = CalendarToQL(quantra::requireEnum(entry->calendar(), entryPath + ".calendar"));
        o.added = parseDates(entry->added_holidays(), entryPath + ".added_holidays");
        o.removed = parseDates(entry->removed_holidays(), entryPath + ".removed_holidays");
        out.push_back(std::move(o));
    }
    return out;
}

void applyRequestCalendarOverrides(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::CalendarOverride>>* overrides,
    const std::string& path) {
    if (overrides == nullptr) return;
    applyCalendarOverrides(parseCalendarOverrides(overrides, path), path);
}

} // namespace quantra
