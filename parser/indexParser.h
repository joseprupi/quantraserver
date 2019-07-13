//
// Created by Josep Rubio on 6/13/17.
//

#ifndef QUANTRASERVER_INDEXPARSER_H
#define QUANTRASERVER_INDEXPARSER_H

#include "crow.h"
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"

class indexParser: QuantraParser{

private:

    //std::vector<std::string> errorLog;
    std::string message;

    Period period;
    int settlementDays;
    Calendar fixingCalendar;
    BusinessDayConvention convention;
    bool endOfMonth;
    DayCounter dayCounter;

    boost::shared_ptr<YieldTermStructure> forecastingTermStructure;

    std::vector<boost::shared_ptr<fixing> > fixings;

    boost::shared_ptr<IborIndex> index;

public:
    indexParser(std::vector<std::string> *errorLog, std::string message);
    //bool parse(crow::json::rvalue JSON);
    bool parse(Value &JSON);
    boost::shared_ptr<IborIndex> createIndex();

    void setPeriod(const Period &period);
    void setSettlementDays(int settlementDays);
    void setCalendar(const Calendar &fixingCalendar);
    void setConvention(const BusinessDayConvention &convention);
    void setEndOfMonth(bool indexEndOfMonth);
    void setDayCounter(const DayCounter &indexDayCounter);

    void setForecastingTermStructure(boost::shared_ptr<YieldTermStructure>);

    const Period &getPeriod() const;
    int getSettlementDays() const;
    const Calendar &getFixingCalendar() const;
    const BusinessDayConvention &getConvention() const;
    bool isEndOfMonth() const;
    const DayCounter &getDayCounter() const;

};

#endif //QUANTRASERVER_INDEXPARSER_H
