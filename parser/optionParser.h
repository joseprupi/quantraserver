//
// Created by Josep Rubio on 6/19/17.
//

#ifndef QUANTRASERVER_OPTIONPARSER_H
#define QUANTRASERVER_OPTIONPARSER_H


#include "rapidjson.h"
#include "QuantraParser.h"
#include "../common/enums.h"
#include "europeanOptionParser.h"
#include "europeanMultiOptionParser.h"
#include <document.h>
#include "crow.h"

class optionParser: public QuantraParser {
private:
    boost::shared_ptr<bool> error;
    std::string message;
    std::vector<std::string> *errorLog;

    OptionStyleEnum optionStyle;
    boost::shared_ptr<europeanOptionParser> EuropeanOptionParser;
    boost::shared_ptr<europeanMultiOptionParser> EuropeanMultiOptionParser;

    bool isMultiOption;

public:
    optionParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &JSON);
    OptionStyleEnum getOptionStyle();
    void setMultioption(bool isMultiOption);
    boost::shared_ptr<europeanOptionParser> getEuropeanOptionParser();
    boost::shared_ptr<europeanMultiOptionParser> getEuropeanMultiOptionParser();

};


#endif //QUANTRASERVER_OPTIONPARSER_H
