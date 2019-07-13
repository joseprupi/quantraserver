//
// Created by Josep Rubio on 6/23/17.
//

#include "fwdCurveParser.h"

fwdCurveParser::fwdCurveParser(std::vector<std::string> *errorLog, std::string message)
:QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;

}

void fwdCurveParser::setFwdCurvePeriodNumber(const int &FwdCurvePeriodNumber){
    this->FwdCurvePeriodNumber = FwdCurvePeriodNumber;
}

void fwdCurveParser::setFwdCurvePeriodTimeUnit(const TimeUnit &FwdCurvePeriodTimeUnit){
    this->FwdCurvePeriodTimeUnit= FwdCurvePeriodTimeUnit;
}

int fwdCurveParser::getFwdCurvePeriodNumber(){
    return this->FwdCurvePeriodNumber;
}

TimeUnit fwdCurveParser::getFwdCurvePeriodTimeUnit(){
    return this->FwdCurvePeriodTimeUnit;
}

Calendar fwdCurveParser::getFwdCalendar(){
    return this->calendar;
}

DayCounter fwdCurveParser::getDayCounter(){
    return this->dayCounter;
}

Date fwdCurveParser::getStartDate(){
    return this->startDate;
}

Date fwdCurveParser::getEndDate(){
    return this->endDate;
}

bool fwdCurveParser::getDates(){
    return this->dates;
}

Compounding fwdCurveParser::getCompounding(){
    return this->compounding;
}

bool fwdCurveParser::parse(Value &JSON) {

    bool error = false;

    ostringstream erroros;

    Value::MemberIterator iterator;

    iterator = JSON.FindMember("PeriodNumber");
    if(iterator != JSON.MemberEnd()){
        if(this->qGetInt(JSON, "PeriodNumber", &FwdCurvePeriodNumber, true)){
            error = true;
        }

        if(this->qTimeUnit(JSON, "PeriodTimeUnit", &FwdCurvePeriodTimeUnit, true)){
            error = true;
        }
    }else{
        iterator = JSON.FindMember("StartDate");
        if(iterator != JSON.MemberEnd()){
            if(this->qGetRawDate(JSON, "StartDate", &startDate, true)){
                error = true;
            }

            if(this->qGetRawDate(JSON, "EndDate", &endDate, true)){
                error = true;
            }
            dates = true;
        }else{
            erroros << "Either dates or a period needs to be specified ";
            CROW_LOG_DEBUG << "Either dates or a period needs to be specified ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }
    }

    if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
        error = true;
    }

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    if(this->qCompounding(JSON, "Compounding", &compounding, true)){
        error = true;
    }

    return error;

}