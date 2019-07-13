//
// Created by Josep Rubio on 6/19/17.
//

#include "yieldParser.h"

yieldParser::yieldParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;

}

void yieldParser::setDayCounter(const DayCounter &dayCounter) {
    this->dayCounter = dayCounter;
}

void yieldParser::setComp(const Compounding &comp) {
    this->comp = comp;
}

void yieldParser::setFrequency(Frequency frequency){
    this->frequency = frequency;
}

DayCounter yieldParser::getDayCounter(){
    return this->dayCounter;
}

Compounding yieldParser::getCompounding(){
    return this->comp;
}

Frequency yieldParser::getFrequency(){
    this->frequency;
}

bool yieldParser::parse(Value &JSON) {

    bool error = false;

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    if(this->qCompounding(JSON, "Compounding", &comp, true)){
        error = true;
    }

    if(this->qGetPeriod(JSON, "Frequency", &frequency, true)){
        error = true;
    }

    return error;

}