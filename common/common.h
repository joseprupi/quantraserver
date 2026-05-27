#include <ql/quantlib.hpp>
#include "error.h"

#ifndef COMMON_H
#define COMMON_H

QuantLib::Date DateToQL(std::string date);

/// QuantLib::Date → ISO-8601 string ("YYYY-MM-DD"). Reverse of DateToQL.
std::string DateToIso(const QuantLib::Date& date);

#endif //COMMON_H