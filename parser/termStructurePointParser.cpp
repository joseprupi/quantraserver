//
// Created by Josep Rubio on 6/18/17.
//

#include "termStructurePointParser.h"
#include "termStructureParser.h"

termStructurePointParser::termStructurePointParser(std::string message, std::vector<std::string> *errorLog,
                                                   std::map<string,boost::shared_ptr<termStructureParser>> termStructuresList)
        :QuantraParser(message, errorLog){

    this->termStructuresList = termStructuresList;
    this->message = message;
    this->errorLog = errorLog;

}

void termStructurePointParser::setRate(Rate rate){
    this->rate = rate;
}

void termStructurePointParser::setType(PointType type){
    this->type = type;
}

void termStructurePointParser::setTenorTimeUnit(TimeUnit tenorTimeUnit){
    this->tenorTimeUnit = tenorTimeUnit;
}

void termStructurePointParser::setTenorNumber(int tenorNumber){
    this->tenorNumber = tenorNumber;
}

void termStructurePointParser::setFixingDays(Natural fixingDays){
    this->fixingDays = fixingDays;
}

void termStructurePointParser::setCalendar(Calendar calendar){
    this->calendar = calendar;
}

void termStructurePointParser::setConvention(BusinessDayConvention convention){
    this->convention = convention;
}

void termStructurePointParser::setDayCounter(DayCounter dayCounter) {
    this->dayCounter = dayCounter;
}

void termStructurePointParser::setMonthsToStart(int MonthsToStart){
    this->MonthsToStart = MonthsToStart;
}

void termStructurePointParser::setMonthsToEnd(int MonthsToEnd){
    this->MonthsToEnd = MonthsToEnd;
}

void termStructurePointParser::setSwFixedLegFrequency(Frequency swFixedLegFrequency){

    this->swFixedLegFrequency = swFixedLegFrequency;

}

void termStructurePointParser::setSwFixedLegConvention(BusinessDayConvention swFixedLegConvention){

    this->swFixedLegConvention = swFixedLegConvention;

}

void termStructurePointParser::setSwFixedLegDayCounter(DayCounter swFixedLegDayCounter){

    this->swFixedLegDayCounter = swFixedLegDayCounter;

}

void termStructurePointParser::setIndex(boost::shared_ptr<IborIndex> index){

    this->index = index;

}

void termStructurePointParser::setFutMonths(int futMonths){

    this->futMonths = futMonths;

}

Rate termStructurePointParser::getRate() const {
    return rate;
}

const PointType &termStructurePointParser::getType() const {
    return type;
}

const TimeUnit &termStructurePointParser::getTenorTimeUnit() const {
    return tenorTimeUnit;
}

int termStructurePointParser::getTenorNumber() const {
    return tenorNumber;
}

Natural termStructurePointParser::getFixingDays() const {
    return fixingDays;
}

const Calendar &termStructurePointParser::getCalendar() const {
    return calendar;
}

const BusinessDayConvention &termStructurePointParser::getConvention() const {
    return convention;
}

const DayCounter &termStructurePointParser::getDayCounter() const {
    return dayCounter;
}

int termStructurePointParser::getMonthsToStart() const {
    return MonthsToStart;
}

int termStructurePointParser::getMonthsToEnd() const {
    return MonthsToEnd;
}

int termStructurePointParser::getFutMonths() const {
    return futMonths;
}

const Frequency &termStructurePointParser::getSwFixedLegFrequency() const {
    return swFixedLegFrequency;
}

const BusinessDayConvention &termStructurePointParser::getSwFixedLegConvention() const {
    return swFixedLegConvention;
}

const DayCounter &termStructurePointParser::getSwFixedLegDayCounter() const {
    return swFixedLegDayCounter;
}

const boost::shared_ptr<IborIndex> &termStructurePointParser::getIndex() const {
    return index;
}

const boost::shared_ptr<OvernightIndex> &termStructurePointParser::getOvernightIndex() const {
    return overnightIndex;
}

const Date &termStructurePointParser::getFutureStartDate() const {
    return futureStartDate;
}

boost::shared_ptr<scheduleParser> termStructurePointParser::getScheduleParser(){
    return this->ScheduleParser;
}

Real termStructurePointParser::getFaceAmount(){
    return this->faceAmount;
}

Rate termStructurePointParser::getCouponRate(){
    return this->couponRate;
}

Real termStructurePointParser::getRedemption(){
    return this->redemption;
}

Date termStructurePointParser::getIssueDate(){
    return this->issueDate;
}

Date termStructurePointParser::getStartDate(){
    return this->startDate;
}

Date termStructurePointParser::getEndtDate(){
    return this->endDate;
}

bool termStructurePointParser::getExogeneousPoint(){
    return exogeneousPoint;
}

boost::shared_ptr<YieldTermStructure> termStructurePointParser::getTermStructure(){
    return termStructure;
}

void termStructurePointParser::setFutureStartDate(string futureStartDate){
    try {
        this->futureStartDate = DateParser::parseFormatted(futureStartDate, "%Y/%m/%d");
    }catch (Error e) {
        //throw std::runtime_error(futureStartDate + " is not a valid Y/m/d BusinessDayConvention. " + e.what());
        QL_FAIL(futureStartDate + " is not a valid Y/m/d BusinessDayConvention. " + e.what());
    }
}

bool termStructurePointParser::parse(Value &JSON){

    bool error = false;
    ostringstream erroros;

    try {

        if(this->qPointType(JSON, "type", &type, true) == false){

            switch(type) {
                case DepositString: {

                    if (this->qGetDouble(JSON, "Rate", &rate, true)) {
                        error = true;
                    }

                    if (this->qGetInt(JSON, "TenorNumber", &tenorNumber, true)) {
                        error = true;
                    }

                    if (this->qGetInt(JSON, "FixingDays", &fixingDays, true)) {
                        error = true;
                    }

                    if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
                        error = true;
                    }

                    if (this->qTimeUnit(JSON, "TenorTimeUnit", &tenorTimeUnit, true)) {
                        error = true;
                    }

                    if (this->qGetConvention(JSON, "BusinessDayConvention", &convention, true)) {
                        error = true;
                    }

                    if (this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)) {
                        error = true;
                    }

                    break;

                }

                case FraString: {

                    if (this->qGetDouble(JSON, "Rate", &rate, true)) {
                        error = true;
                    }

                    if (this->qGetInt(JSON, "MonthsToStart", &MonthsToStart, true)) {
                        error = true;
                    }

                    if (this->qGetInt(JSON, "MonthsToEnd", &MonthsToEnd, true)) {
                        error = true;
                    }


                    if (this->qGetInt(JSON, "FixingDays", &fixingDays, true)) {
                        error = true;
                    }

                    if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
                        error = true;
                    }

                    if (this->qGetConvention(JSON, "BusinessDayConvention", &convention, true)) {
                        error = true;
                    }

                    if (this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)) {
                        error = true;
                    }

                    break;

                }

                case FutureString: {

                    if (this->qGetDouble(JSON, "Rate", &rate, true)) {
                        error = true;
                    }

                    if (this->qGetRawDate(JSON, "FutureStartDate", &futureStartDate, true)) {
                        error = true;
                    }

                    if (this->qGetInt(JSON, "FutMonths", &futMonths, true)) {
                        error = true;
                    }


                    if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
                        error = true;
                    }

                    if (this->qGetConvention(JSON, "BusinessDayConvention", &convention, true)) {
                        error = true;
                    }

                    if (this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)) {
                        error = true;
                    }

                    break;

                }

                case SwapString: {

                    if (this->qGetDouble(JSON, "Rate", &rate, true)) {
                        error = true;
                    }

                    if (this->qGetInt(JSON, "TenorNumber", &tenorNumber, true)) {
                        error = true;
                    }

                    if (this->qTimeUnit(JSON, "TenorTimeUnit", &tenorTimeUnit, true)) {
                        error = true;
                    }

                    if (this->qGetCalendar(JSON, "Calendar", &calendar, true)) {
                        error = true;
                    }

                    if (this->qGetPeriod(JSON, "SwFixedLegFrequency", &swFixedLegFrequency, true)) {
                        error = true;
                    }

                    if (this->qGetConvention(JSON, "SwFixedLegConvention", &swFixedLegConvention, true)) {
                        error = true;
                    }

                    if (this->qGetDayCounter(JSON, "SwFixedLegDayCounter", &swFixedLegDayCounter, true)) {
                        error = true;
                    }

                    if (this->qIborIndex(JSON, "SwFloatingLegIndex", this->index, true)) {
                        error = true;
                    }

                    Value::MemberIterator iterator;

                    iterator = JSON.FindMember("DiscountingTermStructure");
                    if (iterator != JSON.MemberEnd()) {
                        bool tmpError = false;
                        string discountingTermStructureJSON;

                        if (this->qGetString(JSON, "DiscountingTermStructure", &discountingTermStructureJSON, true)){
                            tmpError = true;
                        }

                        if (!tmpError) {
                            std::map<string,boost::shared_ptr<termStructureParser>>::iterator it;
                            it = termStructuresList.find(discountingTermStructureJSON);
                            if (it != termStructuresList.end()) {
                                termStructure = it->second->getTermStructure();
                                exogeneousPoint = true;
                            }else{
                                erroros << "Exogeneous discount curve not found ";
                                CROW_LOG_DEBUG << "Exogeneous discount curve not found ";
                                errorLog->push_back(erroros.str());
                                erroros.str("");
                                erroros.clear();
                                error = true;
                            }

                        } else {
                            error = true;
                        }
                    }

                    break;

                }

                case BondString: {

                    if (this->qGetDouble(JSON, "Rate", &rate, true)) {
                        error = true;
                    }

                    if (this->qGetInt(JSON, "FixingDays", &fixingDays, true)) {
                        error = true;
                    }

                    if (this->qGetDouble(JSON, "FaceAmount", &faceAmount, true)) {
                        error = true;
                    }

                    Value::MemberIterator iterator;
                    iterator = JSON.FindMember("Schedule");
                    if (iterator != JSON.MemberEnd()) {
                        Value &scheduleJSON = JSON["Schedule"];
                        this->ScheduleParser = boost::shared_ptr<scheduleParser>(
                                new scheduleParser(this->getErrorLog(), "Bond Schedule", false));
                        error = error || ScheduleParser->parse(scheduleJSON);
                    } else {
                        erroros << "Bond Schedule: Not found ";
                        CROW_LOG_DEBUG << "Bond Schedule: Not found ";
                        errorLog->push_back(erroros.str());
                        erroros.str("");
                        erroros.clear();
                        error = true;
                    }

                    if (this->qGetDouble(JSON, "CouponRate", &couponRate, true)) {
                        error = true;
                    }

                    if (this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)) {
                        error = true;
                    }

                    if (this->qGetConvention(JSON, "BusinessDayConvention", &convention, true)) {
                        error = true;
                    }

                    if (this->qGetDouble(JSON, "Redemption", &redemption, true)) {
                        error = true;
                    }

                    if (this->qGetRawDate(JSON, "IssueDate", &issueDate, true)) {
                        error = true;
                    }

                    break;

                }
                case OISString: {

                    if(this->qGetInt(JSON, "FixingDays", &fixingDays, true)){
                        error = true;
                    }

                    if(this->qGetInt(JSON, "TenorNumber", &tenorNumber, true)){
                        error = true;
                    }

                    if(this->qTimeUnit(JSON, "TenorTimeUnit", &tenorTimeUnit, true)){
                        error = true;
                    }

                    if (this->qGetDouble(JSON, "Rate", &rate, true)) {
                        error = true;
                    }

                    if (this->qGetOvernightIndex(JSON, "OvernightIndex", this->overnightIndex, true)) {
                        error = true;
                    }

                    break;
                }
                case DatedOISString: {

                    if (this->qGetRawDate(JSON, "StartDate", &startDate, true)) {
                        error = true;
                    }

                    if (this->qGetRawDate(JSON, "EndDate", &endDate, true)) {
                        error = true;
                    }

                    if (this->qGetDouble(JSON, "Rate", &rate, true)) {
                        error = true;
                    }

                    if (this->qGetOvernightIndex(JSON, "OvernightIndex", this->overnightIndex, true)) {
                        error = true;
                    }

                    break;
                }
            }
        } else {
            error = true;
        }

    }
    catch (const runtime_error& re) {
        error = true;
        erroros << "Point: "  << re.what();
        CROW_LOG_DEBUG << "Point: " << re.what();
        this->getErrorLog()->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }catch (Error re) {
        error = true;
        erroros << "Point: "  << re.what();
        CROW_LOG_DEBUG << "Point: " << re.what();
        this->getErrorLog()->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;

}