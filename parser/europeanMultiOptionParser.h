//
// Created by Josep Rubio on 6/19/17.
//

#ifndef QUANTRASERVER_EUROPEANMULTIOPTIONPARSER_H
#define QUANTRASERVER_EUROPEANMULTIOPTIONPARSER_H

#include "crow.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include <document.h>

class europeanMultiOptionParser: public QuantraParser {
private:
    std::string message;

    std::vector<Date> maturities;
    std::vector<Rate> underlyings;

    Option::Type type;
    Real strike;
    Calendar calendar;
    Date settlement;
    DayCounter dayCounter;

    string riskFreeRateTermStructureName;
    string dividendYieldTermStructureName;

    boost::shared_ptr<YieldTermStructure> riskFreeRateTermStructure;
    boost::shared_ptr<YieldTermStructure> dividendYieldTermStructure;

public:
    europeanMultiOptionParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);
    string getRiskFreeRateTermStructureName();
    string getDividendYieldTermStructureName();
    std::vector<Rate> getUnderlyings();
    Option::Type getType();
    Real getStrike();
    std::vector<Date> getMaturities();
};


#endif //QUANTRASERVER_EUROPEANMULTIOPTIONPARSER_H
