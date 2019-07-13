//
// Created by Josep Rubio on 6/25/17.
//

#ifndef QUANTRASERVER_COMMON_H
#define QUANTRASERVER_COMMON_H

#include "enums.h"

enum FlowType { PastInterest, Interest, Notional };

struct curvePoint{
    int point;
    std::string date;
    double rate;
};

struct flowResult {
    Date date;
    Real amount;
    Date accrualStartDate;
    Date accrualEndDate;
    Rate discount;
    Rate fixing;
    Real price;
    FlowType flowType;
};

struct bondPrice{
    Real NPV;
    Real cleanPrice;
    Real dirtyPrice;
    Real accruedAmount;
    Rate yield;
    int accruedDays;
    Time macaulayDuration;
    Time modifiedDuration;
    Real convexity;
    Real BPS;
    std::vector<boost::shared_ptr<flowResult> > flows;
};

struct swapPrice{
    Real NPV;
    Spread fairSpread;
    Rate fairRate;
    int accruedDays;
    std::vector<boost::shared_ptr<flowResult> > fixedFlows;
    std::vector<boost::shared_ptr<flowResult> > floatingFlows;
};

struct fixing{
    Date date;
    Rate rate;
};

#endif //QUANTRASERVER_COMMON_H
