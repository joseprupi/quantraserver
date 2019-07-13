//
// Created by Josep Rubio on 6/20/17.
//

#include "pricingParser.h"

pricingParser::pricingParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;

}

Date pricingParser::getAsOfDate(){
    return asOfDate;
}

bool pricingParser::parse(Value &JSON) {

    std::ostringstream erroros;
    bool error = false;

    if (this->qGetRawDate(JSON, "AsOfDate", &asOfDate, true)) {
        error = true;
    }

    return error;

}