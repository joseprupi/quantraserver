//
// Created by Josep Rubio on 7/4/17.
//

#include "floatingLegParser.h"

floatingLegParser::floatingLegParser(std::vector<std::string> *errorLog, std::string message, bool genericDates)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->genericDates = genericDates;
    this->errorLog = errorLog;

}

void floatingLegParser::setDayCounter(const DayCounter &dayCounter) {
    this->dayCounter = dayCounter;
}

DayCounter floatingLegParser::getDayCounter(){
    return this->dayCounter;
}

void floatingLegParser::setSpread(Rate spread) {
    this->spread = spread;
}

Rate floatingLegParser::getSpread(){
    return this->spread;
}

boost::shared_ptr<scheduleParser> floatingLegParser::getSchedule(){
    return this->ScheduleParser;
}

boost::shared_ptr<indexParser> floatingLegParser::getIndexParser(){
    return IndexParser;
}

bool floatingLegParser::parse(Value &JSON) {

    bool error = false;

    ostringstream erroros;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Schedule");
    if(iterator != JSON.MemberEnd()){
        Value &scheduleJSON = JSON["Schedule"];
        this->ScheduleParser = boost::shared_ptr<scheduleParser>(
                new scheduleParser(this->getErrorLog(), "FloatingLeg Schedule", genericDates));
        error = error || ScheduleParser->parse(scheduleJSON);
    }else{
        erroros << "FloatingLeg Schedule: Not found ";
        CROW_LOG_DEBUG << "FloatingLeg Schedule: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    iterator = JSON.FindMember("Index");
    if(iterator != JSON.MemberEnd()){
        Value& indexJSON = JSON["Index"];
        this->IndexParser = boost::shared_ptr<indexParser>(new indexParser(this->getErrorLog(), "FloatingLeg Index"));
        error = error || IndexParser->parse(indexJSON);
    }else{
        erroros << "FloatingLeg Index: Not found ";
        CROW_LOG_DEBUG << "FloatingLeg Index: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    if(this->qGetDouble(JSON, "Spread", &spread, true)){
        error = true;
    }

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    return error;

}