//
// Created by Josep Rubio on 7/3/17.
//

#ifndef QUANTRASERVER_FIXEDRATEBONDPARSER_H
#define QUANTRASERVER_FIXEDRATEBONDPARSER_H

#include "crow.h"
#include <document.h>
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "scheduleParser.h"
#include "indexParser.h"
#include "yieldParser.h"

class fixedRateBondParser: QuantraParser {
private:

    boost::shared_ptr<bool> error;
    std::string message;
    std::vector<std::string> *errorLog;

    string discountingTermStructureName;
    shared_ptr<FixedRateBond> fixedRateBond;

    Integer settlementDays;
    Real faceAmount;
    Rate rate;
    DayCounter dayCounter;
    BusinessDayConvention convention;
    Real redemption;
    Date issueDate;

    Real NPV;

    boost::shared_ptr<yieldParser> YieldParser;
    boost::shared_ptr<scheduleParser> ScheduleParser;

    boost::shared_ptr<YieldTermStructure> discountingTermStructure;
    RelinkableHandle<YieldTermStructure> termStructure;

public:

    fixedRateBondParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &str);
    shared_ptr<FixedRateBond> createFixedRateBond();
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

    void setSettlementDays(Integer settlementDays);
    void setFaceAmount(Real faceAmount);
    void setRate(Rate rate);
    void setDayCounter(const DayCounter &dayCounter);
    void setConvention(const BusinessDayConvention &convention);
    void setRedemption(Real redemption);
    void setIssueDate(const Date &issueDate);

    void setDiscountingTermStructure(boost::shared_ptr<YieldTermStructure>);
};


#endif //QUANTRASERVER_FIXEDRATEBONDPARSER_H
