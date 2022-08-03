#include "common.h"

QuantLib::Date DateToQL(std::string date)
{
    return QuantLib::DateParser::parseFormatted(date, "%Y/%m/%d");
}