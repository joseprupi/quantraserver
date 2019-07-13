//
// Created by Josep Rubio on 6/19/17.
//

#include "europeanMultiOptionParser.h"

europeanMultiOptionParser::europeanMultiOptionParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;

}

std::vector<Date> europeanMultiOptionParser::getMaturities(){
    return this->maturities;
}

string europeanMultiOptionParser::getRiskFreeRateTermStructureName(){
    return this->riskFreeRateTermStructureName;
}
string europeanMultiOptionParser::getDividendYieldTermStructureName(){
    return this->dividendYieldTermStructureName;
}

std::vector<Rate> europeanMultiOptionParser::getUnderlyings(){
    return this->underlyings;
}

Real europeanMultiOptionParser::getStrike(){
    return this->strike;
}

Option::Type europeanMultiOptionParser::getType(){
    return this->type;
}

bool europeanMultiOptionParser::parse(Value &JSON) {

    bool error = false;
    std::ostringstream erroros;

    if(this->qGetOptionType(JSON, "Type", &type, true)){
        error = true;
    }

    /*if(this->qGetDouble(JSON, "Underlying", &underlying, true)){
        error = true;
    }*/

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Underlyings");
    if (iterator != JSON.MemberEnd()) {

        if (!JSON["Underlyings"].IsArray() || JSON["Underlyings"].GetArray().Size() == 0) {
            CROW_LOG_DEBUG << "Underlyings has to be an array with at least 1 element";
            erroros << "Underlyings has to be an array with at least 1 element";
            this->getErrorLog()->push_back(erroros.str());
            error = true;
        } else {

            double underlyings_array[JSON["Underlyings"].GetArray().Size()];

            if(this->qGetDoubleArray(JSON, "Underlyings", underlyings_array, true)){
                error = true;
            }else{
                for(int i = 0; i < JSON["Underlyings"].GetArray().Size(); i++){
                    this->underlyings.push_back(underlyings_array[i]);
                }
            }

        }

    }else {
        erroros << "Underlyings: Not found ";
        CROW_LOG_DEBUG << "Underlyings: Not found ";
        this->getErrorLog()->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    if(this->qGetDouble(JSON, "Strike", &strike, true)){
        error = true;
    }

    if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
        error = true;
    }

    /*if (this->qGetDate(JSON, "Maturity", &maturity, true, calendar)) {
        error = true;
    }*/

    iterator = JSON.FindMember("Maturities");
    if (iterator != JSON.MemberEnd()) {

        if (!JSON["Maturities"].IsArray() || JSON["Maturities"].GetArray().Size() == 0) {
            CROW_LOG_DEBUG << "Maturities has to be an array with at least 1 element";
            erroros << "Maturities has to be an array with at least 1 element";
            this->getErrorLog()->push_back(erroros.str());
            error = true;
        } else {

            Date expirations_array[JSON["Maturities"].GetArray().Size()];

            if(this->qGetDateArray(JSON, "Maturities", expirations_array, true)){
                error = true;
            }else{
                for(int i = 0; i < JSON["Maturities"].GetArray().Size(); i++){
                    this->maturities.push_back(expirations_array[i]);
                }
            }

        }

    }else {
        erroros << "Maturities: Not found ";
        CROW_LOG_DEBUG << "Maturities: Not found ";
        this->getErrorLog()->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
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