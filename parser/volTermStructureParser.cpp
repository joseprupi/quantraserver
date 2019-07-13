//
// Created by Josep Rubio on 6/23/17.
//

#include "volTermStructureParser.h"

volTermStructureParser::volTermStructureParser(std::vector<std::string> *errorLog, std::string message)
:QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;

}

void volTermStructureParser::setDate(const Date &date){
    this->date = date;
}

void volTermStructureParser::setCalendar(const Calendar &calendar){
    this->calendar = calendar;
}

void volTermStructureParser::setDayCounter(const DayCounter &dayCounter){
    this->dayCounter = dayCounter;
}

void volTermStructureParser::setVolatility(const Volatility &volatility){
    this->volatility = volatility;
}

std::vector<Date> volTermStructureParser::getExpirations(){
    return this->expirations;
}

bool volTermStructureParser::parse(Value &JSON) {

    std::ostringstream erroros;

    Value::MemberIterator iterator;
    Value::MemberIterator iterator2;

    bool error = false;

    if (this->qGetVolType(JSON, "VolatilityType", &volType, true)) {
        error = true;
    }

    if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
        error = true;
    }

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    if(volType == BlackConstantVolString){
        if (this->qGetDate(JSON, "Date", &date, true, calendar)) {
            error = true;
        }

        if(this->qGetDouble(JSON, "Volatility", &volatility, true)){
            error = true;
        }
    }else if(volType == BlackVarianceSurfaceString){

        if(this->qInterpolator(JSON, "Interpolator", &interpolator, true)){
            error = true;
        }

        iterator = JSON.FindMember("Expirations");
        if (iterator != JSON.MemberEnd()) {

            if (!JSON["Expirations"].IsArray() || JSON["Expirations"].GetArray().Size() == 0) {
                CROW_LOG_DEBUG << "Expirations has to be an array with at least 1 element";
                erroros << "Expirations has to be an array with at least 1 element";
                this->getErrorLog()->push_back(erroros.str());
                error = true;
            } else {

                Date expirations_array[JSON["Expirations"].GetArray().Size()];

                if(this->qGetDateArray(JSON, "Expirations", expirations_array, true)){
                    error = true;
                }else{
                    for(int i = 0; i < JSON["Expirations"].GetArray().Size(); i++){
                        this->expirations.push_back(expirations_array[i]);
                    }
                }

            }

        }else {
            erroros << "Expirations: Not found ";
            CROW_LOG_DEBUG << "Expirations: Not found ";
            this->getErrorLog()->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }

        iterator = JSON.FindMember("Strikes");
        if (iterator != JSON.MemberEnd()) {

            if (!JSON["Strikes"].IsArray() || JSON["Strikes"].GetArray().Size() == 0) {
                CROW_LOG_DEBUG << "Strikes has to be an array with at least 1 element";
                erroros << "Strikes has to be an array with at least 1 element";
                this->getErrorLog()->push_back(erroros.str());
                error = true;
            } else {

                double strikes_array[JSON["Strikes"].GetArray().Size()];

                if(this->qGetDoubleArray(JSON, "Strikes", strikes_array, true)){
                    error = true;
                }else{
                    for(int i = 0; i < JSON["Strikes"].GetArray().Size(); i++){
                               this->strikes.push_back(strikes_array[i]);
                    }
                }

            }
        }else {
            erroros << "Strikes: Not found ";
            CROW_LOG_DEBUG << "Strikes: Not found ";
            this->getErrorLog()->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }

        iterator = JSON.FindMember("VolMatrix");
        if (iterator != JSON.MemberEnd()) {

            if (!JSON["VolMatrix"].IsArray()) {
                CROW_LOG_DEBUG << "VolMatrix has to be an array";
                erroros << "Expirations has to be an array";
                this->getErrorLog()->push_back(erroros.str());
                error = true;
            } else if (JSON["VolMatrix"].GetArray().Size() != strikes.size()) {
                CROW_LOG_DEBUG << "VolMatrix has to have same length than strikes";
                erroros << "VolMatrix has to have same length than strikes";
                this->getErrorLog()->push_back(erroros.str());
                error = true;
            } else {
                int i = 0;

                volMatrix = new Matrix(strikes.size(), expirations.size());
                //Matrix volMatrix(strikes.size(), expirations.size());

                while (i < JSON["VolMatrix"].Size()) {


                    if (!JSON["VolMatrix"][i].IsArray()) {
                        CROW_LOG_DEBUG << "VolMatrix[" + std::to_string(i) << "] has to be an array";
                        erroros << "VolMatrix[" + std::to_string(i) << "] has to be an array";
                        this->getErrorLog()->push_back(erroros.str());
                        error = true;
                    } else if (JSON["VolMatrix"][i].GetArray().Size() != expirations.size()) {
                        CROW_LOG_DEBUG << "VolMatrix[" + std::to_string(i) << "] has to have same length than expirations";
                        erroros << "VolMatrix[" + std::to_string(i) << "] has to have same length than expirations";
                        this->getErrorLog()->push_back(erroros.str());
                        error = true;
                    } else {
                        double strikes_array[JSON["VolMatrix"][i].GetArray().Size()];

                        for (int j = 0; j <JSON["VolMatrix"][i].Size(); j++){

                            if(JSON["VolMatrix"][i][j].IsNumber()){
                                double tmpDouble = JSON["VolMatrix"][i][j].GetDouble();
                                //*this->volMatrix[i][j] = tmpDouble;
                                (*volMatrix)[i][j] = tmpDouble;
                            }else{
                                error = true;
                                erroros  << message << " "  << ": Not a double number";
                                CROW_LOG_DEBUG  << message << " "  << ": Not an double number";
                                errorLog->push_back(erroros.str());
                                erroros.str("");
                                erroros.clear();
                            }
                        }
                    }

                    i++;
                }
            }

        }else {
            erroros << "VolMatrix: Not found ";
            CROW_LOG_DEBUG << "VolMatrix: Not found ";
            this->getErrorLog()->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }
    }

    return error;
}

boost::shared_ptr<BlackVolTermStructure> volTermStructureParser::createVolTermStructure() {

    try {

        //Settings::instance().evaluationDate() = this->date;

        boost::shared_ptr<BlackVolTermStructure> volTermStructure;

        if(volType == BlackConstantVolString){
            volTermStructure = boost::shared_ptr<BlackVolTermStructure>(
                    new BlackConstantVol(
                            this->date,
                            this->calendar,
                            this->volatility,
                            this->dayCounter));
        }else if(volType == BlackVarianceSurfaceString){
            volTermStructure = boost::shared_ptr<BlackVolTermStructure>(
                    new BlackVarianceSurface(
                            this->date,
                            this->calendar,
                            this->expirations,
                            this->strikes,
                            *this->volMatrix,
                            this->dayCounter));

            boost::shared_ptr<BlackVarianceSurface> term =
                    boost::dynamic_pointer_cast<BlackVarianceSurface>(volTermStructure);

            term->enableExtrapolation(true);
            
            if(this->interpolator == BiLinearString){
                term->setInterpolation<Bilinear>();
            }else if(this->interpolator == BiCubicString){
                term->setInterpolation<Bicubic>();
            }

        }

        return volTermStructure;

    } catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        QL_FAIL(s);
    }

}