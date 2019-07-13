//
// Created by Josep Rubio on 3/7/18.
//

#ifndef QUANTRASERVER_ZEROCURVESPREADPARSER_H
#define QUANTRASERVER_ZEROCURVESPREADPARSER_H

#include "crow.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include <document.h>

class zeroCurveSpreadParser:public QuantraParser {

private:
    std::string message;
    std::vector<std::string> *errorLog;

    ZeroSpreadTypeEnum zeroSpreadType;
    boost::shared_ptr<Quote> simpleSpread;

    std::vector<Date> spreadDates;
    std::vector<Handle<Quote>> spreadRates;

public:
    zeroCurveSpreadParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);

    ZeroSpreadTypeEnum getZeroSpreadType();
    boost::shared_ptr<Quote> getSimpleSpread();
    std::vector<Handle<Quote>> getSpreadRates();
    std::vector<Date> getSpreadDates();

};


#endif //QUANTRASERVER_ZEROCURVESPREADPARSER_H
