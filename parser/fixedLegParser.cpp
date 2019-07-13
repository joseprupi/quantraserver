//
// Created by Josep Rubio on 7/4/17.
//

#include "fixedLegParser.h"

fixedLegParser::fixedLegParser(std::vector<std::string> *errorLog, std::string message, bool genericDates)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;
    this->genericDates = genericDates;

}

void fixedLegParser::setDayCounter(const DayCounter &dayCounter) {
    this->dayCounter = dayCounter;
}

DayCounter fixedLegParser::getDayCounter(){
    return this->dayCounter;
}

void fixedLegParser::setRate(Rate rate) {
    this->rate = rate;
}

Rate fixedLegParser::getRate(){
    return this->rate;
}

boost::shared_ptr<scheduleParser> fixedLegParser::getSchedule(){
    return this->ScheduleParser;
}

bool fixedLegParser::parse(Value &JSON) {

    bool error = false;

    ostringstream erroros;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Schedule");
    if(iterator != JSON.MemberEnd()){
        Value &scheduleJSON = JSON["Schedule"];
        this->ScheduleParser = boost::shared_ptr<scheduleParser>(
                new scheduleParser(this->getErrorLog(), "FixedLeg Schedule", genericDates));
        error = error || ScheduleParser->parse(scheduleJSON);
    }else{
        erroros << "FixedLeg Schedule: Not found ";
        CROW_LOG_DEBUG << "FixedLeg Schedule: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    if(this->qGetDouble(JSON, "Rate", &rate, true)){
        error = true;
    }

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    return error;

}