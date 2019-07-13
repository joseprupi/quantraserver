//
// Created by Josep Rubio on 6/23/17.
//

#ifndef QUANTRASERVER_FWDCURVEPARSER_H
#define QUANTRASERVER_FWDCURVEPARSER_H

#include "crow.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include <document.h>

class fwdCurveParser: public QuantraParser {
private:
    std::string message;
    std::vector<std::string> *errorLog;

    bool dates = false;

    int  FwdCurvePeriodNumber;
    TimeUnit FwdCurvePeriodTimeUnit;
    Date startDate;
    Date endDate;
    Calendar calendar;
    DayCounter dayCounter;
    Compounding compounding;

public:
    fwdCurveParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);

    void setFwdCurvePeriodNumber(const int &dayCounter);
    void setFwdCurvePeriodTimeUnit(const TimeUnit &FwdCurvePeriodTimeUnit);

    int getFwdCurvePeriodNumber();
    TimeUnit getFwdCurvePeriodTimeUnit();
    Calendar getFwdCalendar();
    DayCounter getDayCounter();
    Date getStartDate();
    Date getEndDate();
    bool getDates();
    Compounding getCompounding();
};



#endif //QUANTRASERVER_FWDCURVEPARSER_H
