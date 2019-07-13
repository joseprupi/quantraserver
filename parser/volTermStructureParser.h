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

class volTermStructureParser: public QuantraParser {
private:
    std::string message;
    std::vector<std::string> *errorLog;

    VolEnum volType;

    Date date;
    Calendar calendar;
    DayCounter dayCounter;
    Volatility volatility;
    InterpolationEnum interpolator;

    std::vector<Date> expirations;
    std::vector<Rate> strikes;

    Matrix *volMatrix;

    boost::shared_ptr<BlackVolTermStructure> volTermStructure;

    Date asOfDate;

public:
    volTermStructureParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);

    void setDate(const Date &date);
    void setCalendar(const Calendar &calendar);
    void setDayCounter(const DayCounter &dayCounter);
    void setVolatility(const Volatility &volatility);
    std::vector<Date> getExpirations();


    boost::shared_ptr<BlackVolTermStructure> createVolTermStructure();

};



#endif //QUANTRASERVER_FWDCURVEPARSER_H
