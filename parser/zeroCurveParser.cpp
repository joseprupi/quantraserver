//
// Created by Josep Rubio on 8/28/17.
//

#include "zeroCurveParser.h"

zeroCurveParser::zeroCurveParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;

}

DayCounter zeroCurveParser::getDayCounter(){
    return this->dayCounter;
}

Compounding zeroCurveParser::getCompounding(){
    return this->compounding;
}

bool zeroCurveParser::parse(Value &JSON) {

    bool error = false;

    ostringstream erroros;

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    if(this->qCompounding(JSON, "Compounding", &compounding, true)){
        error = true;
    }

    return error;

}