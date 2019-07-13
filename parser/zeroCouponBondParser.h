//
// Created by Josep Rubio on 6/27/17.
//

#ifndef QUANTRASERVER_ZEROCOUPONBONDPARSER_H
#define QUANTRASERVER_ZEROCOUPONBONDPARSER_H

#include "crow.h"
#include <document.h>
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "yieldParser.h"

class zeroCouponBondParser: QuantraParser {

private:

    boost::shared_ptr<bool> error;
    std::string message;
    std::vector<std::string> *errorLog;


    string discountingTermStructureName;
    Date issueDate;
    Date maturityDate;
    Integer settlementDays;
    Real faceAmount;
    Real redemption;
    Calendar calendar;
    BusinessDayConvention paymentConvention;

    boost::shared_ptr<yieldParser> YieldParser;

    RelinkableHandle<YieldTermStructure> discountingTermStructure;

    shared_ptr<ZeroCouponBond> zeroCouponBond;

public:
    zeroCouponBondParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &str);
    shared_ptr<ZeroCouponBond> createZeroCouponBond();
    Real getNPV();
    Real getCleanPrice();
    Real getDirtyPrice();
    Real getAccruedAmount();
    Real getYield();
    void getFlows(boost::shared_ptr<bondPrice> bondPrice);
    string getDiscountingTermStructureName();

    void setSettlementDays(Integer);
    void setCalendar(Calendar);
    Calendar getCalendar();
    void setFaceAmount(Real);
    void setMaturityDate(Date);
    void setPaymentConvention(BusinessDayConvention);
    void setRedemption(Real);
    void setIssueDate(Date);

    void setDiscountingTermStructure(boost::shared_ptr<YieldTermStructure> termStructure);

};


#endif //QUANTRASERVER_ZEROCOUPONBONDPARSER_H
