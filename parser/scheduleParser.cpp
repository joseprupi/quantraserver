//
// Created by Josep Rubio on 6/14/17.
//

#include "scheduleParser.h"

scheduleParser::scheduleParser(std::vector<std::string> *errorLog, std::string message, bool genericDates)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->genericDates = genericDates;

}

void scheduleParser::setEffectiveDate(const Date &effectiveDate) {
    this->effectiveDate = effectiveDate;
}

void scheduleParser::setTerminationDate(const Date &terminationDate) {
    this->terminationDate = terminationDate;
}

void scheduleParser::setPeriod(const Frequency &period) {
    this->period = period;
}

void scheduleParser::setCalendar(const Calendar &calendar) {
    this->calendar = calendar;
}

void scheduleParser::setCouponConvention(const BusinessDayConvention &couponConvention) {
    this->couponConvention = couponConvention;
}

void scheduleParser::setTerminationDateConvention(const BusinessDayConvention &terminationDateConvention) {
    this->terminationDateConvention = terminationDateConvention;
}

void scheduleParser::setRule(const DateGeneration::Rule &rule) {
    this->rule = rule;
}

void scheduleParser::setEndOfMonth(bool endOfMonth) {
    this->endOfMonth = endOfMonth;
}

Calendar scheduleParser::getCalendar(){
    return this->calendar;
}

Frequency scheduleParser::getFrequency(){
    return this->period;
}

bool scheduleParser::parse(Value &JSON){

    bool error = false;

    if(!genericDates){
        if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
            error = true;
        }

        if (this->qGetDate(JSON, "EffectiveDate", &effectiveDate, true, calendar)) {
            error = true;
        }

        if (this->qGetDate(JSON, "TerminationDate", &terminationDate, true, calendar)) {
            error = true;
        }
    }

    if(this->qGetPeriod(JSON, "Frequency", &period, false)){
        error = true;
    }

    if(this->qGetConvention(JSON, "Convention", &couponConvention, true)){
        error = true;
    }

    if(this->qGetConvention(JSON, "TerminationDateConvention", &terminationDateConvention, true)){
        error = true;
    }

    if(this->qDateGenerationRule(JSON, "DateGenerationRule", &rule, true)){
        error = true;
    }

    if(this->qBool(JSON, "EndOfMonth", &endOfMonth, true)){
        error = true;
    }

    return error;

}

boost::shared_ptr<Schedule> scheduleParser::createSchedule(){
    try {

        boost::shared_ptr<Schedule> floatingBondSchedule(new Schedule(this->effectiveDate,
                                      this->terminationDate,
                                      Period(this->period),
                                      this->calendar,
                                      this->couponConvention,
                                      this->terminationDateConvention,
                                      this->rule,
                                      this->endOfMonth));

        return floatingBondSchedule;

    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}