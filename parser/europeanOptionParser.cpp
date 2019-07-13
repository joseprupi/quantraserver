//
// Created by Josep Rubio on 6/19/17.
//

#include "europeanOptionParser.h"

europeanOptionParser::europeanOptionParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;

}

Date europeanOptionParser::getMaturity(){
    return this->maturity;
}

string europeanOptionParser::getRiskFreeRateTermStructureName(){
    return this->riskFreeRateTermStructureName;
}
string europeanOptionParser::getDividendYieldTermStructureName(){
    return this->dividendYieldTermStructureName;
}

Real europeanOptionParser::getUnderlying(){
    return this->underlying;
}

Real europeanOptionParser::getStrike(){
    return this->strike;
}

Option::Type europeanOptionParser::getType(){
    return this->type;
}

bool europeanOptionParser::parse(Value &JSON) {

    bool error = false;

    if(this->qGetOptionType(JSON, "Type", &type, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "Underlying", &underlying, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "Strike", &strike, true)){
        error = true;
    }

    if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
        error = true;
    }

    if (this->qGetDate(JSON, "Maturity", &maturity, true, calendar)) {
        error = true;
    }

    if (this->qGetDate(JSON, "Settlement", &settlement, true, calendar)) {
        error = true;
    }

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    if(this->qGetString(JSON, "RiskFreeRateTermStructure", &riskFreeRateTermStructureName, true)){
        error = true;
    }

    if(this->qGetString(JSON, "DividendYieldTermStructure", &dividendYieldTermStructureName, true)){
        error = true;
    }

    return error;

}