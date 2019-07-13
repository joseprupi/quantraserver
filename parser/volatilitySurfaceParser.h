//
// Created by Josep Rubio on 6/19/17.
//

#ifndef QUANTRASERVER_YIELDPARSER_H
#define QUANTRASERVER_YIELDPARSER_H


#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include <document.h>

class volatilitySurfaceParser: public QuantraParser {
private:
    std::string message;

    int expirationStep;
    double strikeStep;

    double initStrike;
    double endStrike;

    Date initDate;
    Date endDate;

public:
    volatilitySurfaceParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);

    int getExpirationStep();
    double getStrikeStep();
    double getInitStrike();
    double getEndStrike();
    Date getInitDate();
    Date getEndDate();
};


#endif //QUANTRASERVER_YIELDPARSER_H
