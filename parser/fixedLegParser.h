//
// Created by Josep Rubio on 7/4/17.
//

#ifndef QUANTRASERVER_FIXEDLEGPARSER_H
#define QUANTRASERVER_FIXEDLEGPARSER_H

#include "crow.h"
#include <document.h>
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "scheduleParser.h"

class fixedLegParser: QuantraParser {
private:

    boost::shared_ptr<bool> error;
    std::string message;
    std::vector<std::string> *errorLog;

    bool genericDates = false;

    boost::shared_ptr<scheduleParser> ScheduleParser;

    Rate rate;
    DayCounter dayCounter;

public:

    fixedLegParser(std::vector<std::string> *errorLog, std::string message, bool genericDates);
    bool parse(Value &str);

    void setDayCounter(const DayCounter &dayCounter);
    void setRate(Rate rate);

    boost::shared_ptr<scheduleParser> getSchedule();
    Rate getRate();
    DayCounter getDayCounter();
};


#endif //QUANTRASERVER_FIXEDLEGPARSER_H
