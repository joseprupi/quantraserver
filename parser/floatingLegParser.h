//
// Created by Josep Rubio on 7/4/17.
//

#ifndef QUANTRASERVER_FLOATINGLEGPARSER_H
#define QUANTRASERVER_FLOATINGLEGPARSER_H

#include "crow.h"
#include <document.h>
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "scheduleParser.h"
#include "indexParser.h"

class floatingLegParser: QuantraParser {
private:

    boost::shared_ptr<bool> error;
    std::string message;
    std::vector<std::string> *errorLog;

    bool genericDates = false;

    boost::shared_ptr<scheduleParser> ScheduleParser;
    boost::shared_ptr<indexParser> IndexParser;

    Rate spread;
    DayCounter dayCounter;

public:

    floatingLegParser(std::vector<std::string> *errorLog, std::string message, bool genericDates);
    bool parse(Value &str);

    void setDayCounter(const DayCounter &dayCounter);
    void setSpread(Rate rate);

    Rate getSpread();
    DayCounter getDayCounter();
    boost::shared_ptr<indexParser> getIndexParser();

    boost::shared_ptr<scheduleParser> getSchedule();
};

#endif //QUANTRASERVER_FLOATINGLEGPARSER_H
