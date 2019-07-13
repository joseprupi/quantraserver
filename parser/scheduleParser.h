//
// Created by Josep Rubio on 6/14/17.
//

#ifndef QUANTRASERVER_SCHEDULEPARSER_H
#define QUANTRASERVER_SCHEDULEPARSER_H

#include "../common/enums.h"
#include <document.h>
#include "rapidjson.h"
#include "QuantraParser.h"

using namespace rapidjson;
class scheduleParser: QuantraParser {

private:

    std::vector<std::string> errorLog;
    std::string message;

    bool genericDates = false;

    Date effectiveDate;
    Date terminationDate;
    Frequency period;
    Calendar calendar;
    BusinessDayConvention couponConvention;
    BusinessDayConvention terminationDateConvention;
    DateGeneration::Rule rule;
    bool endOfMonth;

public:
    scheduleParser(std::vector<std::string> *errorLog, std::string message, bool genericDates);
    bool parse(Value &str);
    boost::shared_ptr<Schedule> createSchedule();

    void setEffectiveDate(const Date &effectiveDate);
    void setTerminationDate(const Date &terminationDate);
    void setPeriod(const Frequency &period);
    void setCalendar(const Calendar &calendar);
    void setCouponConvention(const BusinessDayConvention &couponConvention);
    void setTerminationDateConvention(const BusinessDayConvention &terminationDateConvention);
    void setRule(const DateGeneration::Rule &rule);
    void setEndOfMonth(bool endOfMonth);

    Frequency getFrequency();
    Calendar getCalendar();
};


#endif //QUANTRASERVER_SCHEDULEPARSER_H
