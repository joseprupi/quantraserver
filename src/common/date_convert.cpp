#include "date_convert.h"

#include <sstream>
#include <regex>

QuantLib::Date DateToQL(std::string date)
{
    // The wire date format is ISO-8601 YYYY-MM-DD, strictly. Reject any other
    // shape (slashes, textual months, wrong field widths) up front so callers
    // get a clear 400 instead of a silently normalized or opaque QuantLib error.
    static const std::regex isoShape("^[0-9]{4}-[0-9]{2}-[0-9]{2}$");
    if (!std::regex_match(date, isoShape))
    {
        std::string shown = date.size() > 32 ? date.substr(0, 32) + "..." : date;
        QUANTRA_INVALID_ARGUMENT("Invalid date '" + shown + "': expected YYYY-MM-DD");
    }

    const int year = std::stoi(date.substr(0, 4));
    const int month = std::stoi(date.substr(5, 2));
    const int day = std::stoi(date.substr(8, 2));

    if (month < 1 || month > 12)
    {
        QUANTRA_INVALID_ARGUMENT("Invalid date '" + date + "': expected YYYY-MM-DD");
    }

    // Let QuantLib validate day-of-month (e.g. Feb 30) and the supported year
    // range, but surface any rejection as our named 400 rather than an
    // unhandled exception mapped to a 500.
    try
    {
        return QuantLib::Date(day, static_cast<QuantLib::Month>(month), year);
    }
    catch (const std::exception&)
    {
        QUANTRA_INVALID_ARGUMENT("Invalid date '" + date + "': expected YYYY-MM-DD");
    }

    return QuantLib::Date();  // unreachable; QUANTRA_INVALID_ARGUMENT always throws
}

std::string DateToIso(const QuantLib::Date& date)
{
    std::ostringstream os;
    os << QuantLib::io::iso_date(date);
    return os.str();
}
