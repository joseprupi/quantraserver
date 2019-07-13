//
// Created by Josep Rubio on 8/28/17.
//

#ifndef QUANTRASERVER_ZEROCURVEPARSER_H
#define QUANTRASERVER_ZEROCURVEPARSER_H

#include "crow.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include <document.h>

class zeroCurveParser:public QuantraParser {
private:
    std::string message;
    std::vector<std::string> *errorLog;

    DayCounter dayCounter;
    Compounding compounding;

public:
    zeroCurveParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);

    DayCounter getDayCounter();
    Compounding getCompounding();
};


#endif //QUANTRASERVER_ZEROCURVEPARSER_H
