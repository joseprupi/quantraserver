//
// Created by Josep Rubio on 6/19/17.
//

#ifndef QUANTRASERVER_EUROPEANOPTIONPARSER_H
#define QUANTRASERVER_EUROPEANOPTIONPARSER_H


#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include <document.h>

class europeanOptionParser: public QuantraParser {
private:
    std::string message;

    Option::Type type;
    Real underlying;
    Real strike;
    Calendar calendar;
    Date maturity;
    Date settlement;
    DayCounter dayCounter;

    string riskFreeRateTermStructureName;
    string dividendYieldTermStructureName;

    boost::shared_ptr<YieldTermStructure> riskFreeRateTermStructure;
    boost::shared_ptr<YieldTermStructure> dividendYieldTermStructure;

public:
    europeanOptionParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);
    string getRiskFreeRateTermStructureName();
    string getDividendYieldTermStructureName();
    Real getUnderlying();
    Option::Type getType();
    Real getStrike();


    Date getMaturity();
};


#endif //QUANTRASERVER_EUROPEANOPTIONPARSER_H
