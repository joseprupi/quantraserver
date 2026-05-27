#include "common.h"

#include <sstream>

QuantLib::Date DateToQL(std::string date)
{
    return QuantLib::DateParser::parseFormatted(date, "%Y/%m/%d");
}

std::string DateToIso(const QuantLib::Date& date)
{
    std::ostringstream os;
    os << QuantLib::io::iso_date(date);
    return os.str();
}