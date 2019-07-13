//
// Created by Josep Rubio on 6/14/17.
//

#ifndef QUANTRASERVER_FLOATINGRATEBONDPARSER_H
#define QUANTRASERVER_FLOATINGRATEBONDPARSER_H

#include "crow.h"
#include <document.h>
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "scheduleParser.h"
#include "indexParser.h"
#include "yieldParser.h"

using namespace rapidjson;

class floatingRateBondParser: QuantraParser {
private:

    boost::shared_ptr<bool> error;
    std::string message;
    std::vector<std::string> *errorLog;

    string discountingTermStructureName;
    string forecastingTermStructureName;

    shared_ptr<FloatingRateBond> floatingRateBond;

    Integer settlementDays;
    Real faceAmount;
    DayCounter dayCounter;
    BusinessDayConvention convention;
    int fixingDays;
    Rate spread;
    Real redemption;
    Date issueDate;

    boost::shared_ptr<YieldTermStructure> discountingTermStructure;
    RelinkableHandle<YieldTermStructure> termStructure;
    boost::shared_ptr<YieldTermStructure> forecastingTermStructure;

    boost::shared_ptr<scheduleParser> ScheduleParser;
    boost::shared_ptr<indexParser> IndexParser;
    boost::shared_ptr<yieldParser> YieldParser;

public:
    floatingRateBondParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &str);
    shared_ptr<FloatingRateBond> createFloatingRateBond();
    Real getNPV();
    Real getCleanPrice();
    Real getDirtyPrice();
    Real getAccruedAmount();
    Real getYield();
    int getAccruedDays();
    Time getMacaulayDuration();
    Time getModifiedDuration();
    Real getConvexity();
    Real getBPS();
    void getFlows(boost::shared_ptr<bondPrice> bondPrice);
    string getDiscountingTermStructureName();
    string getForecastingTermStructureName();

    void setSettlementDays(Integer settlementDays);
    void setFaceAmount(Real faceAmount);
    void setDayCounter(const DayCounter &dayCounter);
    void setConvention(const BusinessDayConvention &convention);
    void setFixingDays(int fixingDays);
    void setSpread(Rate spread);
    void setRedemption(Real redemption);
    void setIssueDate(const Date &issueDate);

    void setDiscountingTermStructure(boost::shared_ptr<YieldTermStructure>);
    void setForecastingTermStructure(boost::shared_ptr<YieldTermStructure>);
};


#endif //QUANTRASERVER_FLOATINGRATEBONDPARSER_H
