//
// Created by Josep Rubio on 6/18/17.
//

#ifndef QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H
#define QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H

#include "crow.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include "scheduleParser.h"
//#include "termStructureParser.h"
#include <document.h>

class termStructureParser;

class termStructurePointParser: public QuantraParser {
private:
    std::string message;
    std::vector<std::string> *errorLog;

    Rate rate;
    PointType type;
    TimeUnit tenorTimeUnit;
    int tenorNumber;
    int fixingDays;
    Calendar calendar;
    BusinessDayConvention convention;
    DayCounter dayCounter;
    int MonthsToStart;
    int MonthsToEnd;
    Frequency swFixedLegFrequency;
    BusinessDayConvention swFixedLegConvention;
    DayCounter swFixedLegDayCounter;
    boost::shared_ptr<IborIndex> index;
    boost::shared_ptr<OvernightIndex> overnightIndex;
    int futMonths;
    Date futureStartDate;
    Real faceAmount;
    boost::shared_ptr<scheduleParser> ScheduleParser;
    Rate couponRate;
    Real redemption;
    Date issueDate;
    Date startDate;
    Date endDate;

    bool exogeneousPoint = false;

    boost::shared_ptr<YieldTermStructure> termStructure;

    std::map<string,boost::shared_ptr<termStructureParser>> termStructuresList;

public:
    termStructurePointParser(std::string message, std::vector<std::string> *errorLog,
                             std::map<string,boost::shared_ptr<termStructureParser>> termStructuresList);
    bool parse(Value &JSON);

    void setRate(Rate rate);
    void setType(PointType type);
    void setTenorTimeUnit(TimeUnit tenorTimeUnit);
    void setTenorNumber(int tenorNumber);
    void setFixingDays(Natural fixingDays);
    void setCalendar(Calendar calendar);
    void setConvention(BusinessDayConvention convention);
    void setDayCounter(DayCounter dayCounter);
    void setMonthsToStart(int MonthsToStart);
    void setMonthsToEnd(int MonthsToEnd);
    void setSwFixedLegFrequency(Frequency swFixedLegFrequency);
    void setSwFixedLegConvention(BusinessDayConvention swFixedLegConvention);
    void setSwFixedLegDayCounter(DayCounter swFixedLegDayCounter);
    void setIndex(boost::shared_ptr<IborIndex> index);
    void setFutMonths(int futMonths);
    void setFutureStartDate(string futureStartDate);

    Rate getRate() const;
    const PointType &getType() const;
    const TimeUnit &getTenorTimeUnit() const;
    int getTenorNumber() const;
    Natural getFixingDays() const;
    const Calendar &getCalendar() const;
    const BusinessDayConvention &getConvention() const;
    const DayCounter &getDayCounter() const;
    int getMonthsToStart() const;
    int getMonthsToEnd() const;
    int getFutMonths() const;
    const Frequency &getSwFixedLegFrequency() const;
    const BusinessDayConvention &getSwFixedLegConvention() const;
    const DayCounter &getSwFixedLegDayCounter() const;
    const boost::shared_ptr<IborIndex> &getIndex() const;
    const boost::shared_ptr<OvernightIndex> &getOvernightIndex() const;
    const Date &getFutureStartDate() const;
    boost::shared_ptr<scheduleParser> getScheduleParser();
    Real getFaceAmount();
    Rate getCouponRate();
    Real getRedemption();
    Date getIssueDate();
    Date getStartDate();
    Date getEndtDate();
    bool getExogeneousPoint();
    boost::shared_ptr<YieldTermStructure> getTermStructure();
};


#endif //QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H
