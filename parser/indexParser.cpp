//
// Created by Josep Rubio on 6/13/17.
//

#include "indexParser.h"

indexParser::indexParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;

}

bool indexParser::parse(Value &JSON){

    std::ostringstream erroros;
    bool error = false;
    
    if(this->qGetInt(JSON, "SettlementDays", &settlementDays, true)){
        error = true;
    }

    if(this->qGetCalendar(JSON, "Calendar", &fixingCalendar, true)){
        error = true;
    }

    if(this->qGetConvention(JSON, "Convention", &convention, true)){
        error = true;
    }

    if(this->qBool(JSON, "EndOfMonth", &endOfMonth, true)){
        error = true;
    }

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Fixings");
    if (iterator != JSON.MemberEnd()) {

        if (!JSON["Fixings"].IsArray() || JSON["Fixings"].GetArray().Size() == 0){
            CROW_LOG_DEBUG << "Fixings has to be an array with at least 1 element";
            erroros << "Fixings has to be an array with at least 1 element";
            this->getErrorLog()->push_back(erroros.str());
            error = true;
        }else{
            int i = 0;

            while (i < JSON["Fixings"].Size()) {

                bool fixingError = false;

                boost::shared_ptr<fixing> fixingValue(new fixing());

                if(this->qGetDouble(JSON["Fixings"][i], "Rate", &fixingValue->rate, true)){
                    fixingError = true;
                }

                if(this->qGetRawDate(JSON["Fixings"][i], "Date", &fixingValue->date, true)){
                    fixingError = true;
                }

                if(fixingError == false){
                    fixings.push_back(fixingValue);
                }else{
                    error = true;
                }

                i++;
            }
        }

    }
    
    return error;
}

boost::shared_ptr<IborIndex> indexParser::createIndex(){

    try {

        RelinkableHandle<YieldTermStructure> termStructure;
        termStructure.linkTo(forecastingTermStructure);

        IndexManager::instance().clearHistories();

        index = boost::shared_ptr<IborIndex>(new IborIndex("index",
                                                         Period(this->period),
                                                         this->settlementDays,
                                                         EURCurrency(),
                                                         this->fixingCalendar,
                                                         this->convention,
                                                         this->endOfMonth,
                                                         this->dayCounter,
                                                         termStructure));

        for(std::vector<boost::shared_ptr<fixing>>::iterator it = fixings.begin(); it != fixings.end(); ++it) {
            index->addFixing((*it)->date, (*it)->rate);
        }

        return index;

    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

void indexParser::setPeriod(const Period &period) {
    this->period = period;
}

void indexParser::setSettlementDays(int settlementDays) {
    this->settlementDays = settlementDays;
}

void indexParser::setCalendar(const Calendar &fixingCalendar) {
    this->fixingCalendar = fixingCalendar;
}

void indexParser::setConvention(const BusinessDayConvention &convention) {
    this->convention = convention;
}

void indexParser::setEndOfMonth(bool endOfMonth) {
    this->endOfMonth = endOfMonth;
}

void indexParser::setDayCounter(const DayCounter &dayCounter) {
    this->dayCounter = dayCounter;
}

void indexParser::setForecastingTermStructure(boost::shared_ptr<YieldTermStructure> forecastingTermStructure){
    this->forecastingTermStructure = forecastingTermStructure;
}

const Period &indexParser::getPeriod() const {
    return this->period;
}

int indexParser::getSettlementDays() const {
    return this->settlementDays;
}

const Calendar &indexParser::getFixingCalendar() const {
    return this->fixingCalendar;
}

const BusinessDayConvention &indexParser::getConvention() const {
    return this->convention;
}

bool indexParser::isEndOfMonth() const {
    return this->endOfMonth;
}

const DayCounter &indexParser::getDayCounter() const {
    return this->dayCounter;
}
