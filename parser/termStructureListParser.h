//
// Created by Josep Rubio on 8/28/17.
//

#ifndef QUANTRASERVER_TERMSTRUCTURELISTPARSER_H
#define QUANTRASERVER_TERMSTRUCTURELISTPARSER_H

#include "crow.h"
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include <document.h>
#include "termStructureParser.h"

class termStructureListParser : public QuantraParser {

private:

    std::string message;

    Date asOfDate;
    std::map<string,boost::shared_ptr<termStructureParser>> termStructuresList;
    //boost::shared_ptr<YieldTermStructure> termStructure;

public:

    termStructureListParser(std::string message, std::vector<std::string> *errorLog);
    bool parse(Value &str);

    std::map<string,boost::shared_ptr<termStructureParser>> getTermStructuresList();
    void setAsOfDate(Date date);
};


#endif //QUANTRASERVER_TERMSTRUCTURELISTPARSER_H
