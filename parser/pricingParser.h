//
// Created by Josep Rubio on 6/20/17.
//

#ifndef QUANTRASERVER_PRICINGPARSER_H
#define QUANTRASERVER_PRICINGPARSER_H

#include "../common/enums.h"
#include "rapidjson.h"
#include "QuantraParser.h"

class pricingParser: QuantraParser {

private:

    std::string message;

    Date asOfDate;

public:
    pricingParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);

    Date getAsOfDate();
};


#endif //QUANTRASERVER_PRICINGPARSER_H
