//
// Created by Josep Rubio on 6/19/17.
//

#ifndef QUANTRASERVER_YIELDPARSER_H
#define QUANTRASERVER_YIELDPARSER_H


#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include <document.h>

class yieldParser: public QuantraParser {
private:
    std::string message;

    DayCounter dayCounter;
    Compounding comp;
    Frequency frequency;

public:
    yieldParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);

    void setDayCounter(const DayCounter &dayCounter);
    void setComp(const Compounding &comp);
    void setFrequency(Frequency frequency);

    DayCounter getDayCounter();
    Compounding getCompounding();
    Frequency getFrequency();
};


#endif //QUANTRASERVER_YIELDPARSER_H
