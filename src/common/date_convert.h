#include "error.h"

#include <ql/quantlib.hpp>

#ifndef DATE_CONVERT_H
#define DATE_CONVERT_H

QuantLib::Date DateToQL(std::string date);

/// QuantLib::Date → ISO-8601 string ("YYYY-MM-DD"). Reverse of DateToQL.
std::string DateToIso(const QuantLib::Date& date);

/// True for exactly "YYYY-MM": seven characters, four digits, '-', two digits,
/// the month 01 to 12.
bool IsYearMonth(const std::string& s);

#endif //DATE_CONVERT_H