//
// Created by Josep Rubio on 7/3/17.
//

#include "fixedRateBondParser.h"

fixedRateBondParser::fixedRateBondParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;

}



void fixedRateBondParser::setSettlementDays(Integer settlementDays) {
    this->settlementDays = settlementDays;
}

void fixedRateBondParser::setFaceAmount(Real faceAmount) {
    this->faceAmount = faceAmount;
}

void fixedRateBondParser::setDayCounter(const DayCounter &dayCounter) {
    this->dayCounter = dayCounter;
}

void fixedRateBondParser::setConvention(const BusinessDayConvention &convention) {
    this->convention = convention;
}

void fixedRateBondParser::setRate(Rate rate) {
    this->rate = rate;
}

void fixedRateBondParser::setRedemption(Real redemption) {
    this->redemption = redemption;
}

void fixedRateBondParser::setIssueDate(const Date &issueDate) {
    this->issueDate = issueDate;
}

void fixedRateBondParser::setDiscountingTermStructure(boost::shared_ptr<YieldTermStructure> discountingTermStructure){
    this->discountingTermStructure = discountingTermStructure;
}

string fixedRateBondParser::getDiscountingTermStructureName(){
    return discountingTermStructureName;
}

bool fixedRateBondParser::parse(Value &JSON){

    bool error = false;

    ostringstream erroros;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Schedule");
    if(iterator != JSON.MemberEnd()){
        Value& scheduleJSON = JSON["Schedule"];
        this->ScheduleParser = boost::shared_ptr<scheduleParser>(new scheduleParser(this->getErrorLog(), "FixedRateBond Schedule", false));
        error = error || ScheduleParser->parse(scheduleJSON);
    }else{
        erroros << "FixedRateBond Schedule: Not found ";
        CROW_LOG_DEBUG << "FixedRateBond Schedule: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    if(this->qGetString(JSON, "DiscountingTermStructure", &discountingTermStructureName, true)){
        error = true;
    }

    if(this->qGetInt(JSON, "SettlementDays", &settlementDays, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "FaceAmount", &faceAmount, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "Rate", &rate, true)){
        error = true;
    }

    if(this->qGetDayCounter(JSON, "DayCounter", &dayCounter, true)){
        error = true;
    }

    if(this->qGetConvention(JSON, "Convention", &convention, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "Redemption", &redemption, true)){
        error = true;
    }

    if(this->qGetDate(JSON, "IssueDate", &issueDate, true, this->ScheduleParser->getCalendar())){
        error = true;
    }

    iterator = JSON.FindMember("Yield");
    if(iterator != JSON.MemberEnd()){
        Value& yieldJSON = JSON["Yield"];
        this->YieldParser = boost::shared_ptr<yieldParser>(new yieldParser(this->getErrorLog(), "FixedRateBond Yield"));
        error = error || YieldParser->parse(yieldJSON);
    }else{
        erroros << "FixedRateBond Yield: Not found ";
        CROW_LOG_DEBUG << "FixedRateBond Yield: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    return error;
}

shared_ptr<FixedRateBond> fixedRateBondParser::createFixedRateBond(){

    try {
        termStructure.linkTo(this->discountingTermStructure);

        boost::shared_ptr<PricingEngine> bondEngine(
                new DiscountingBondEngine(termStructure));
        boost::shared_ptr<Schedule> schedule = this->ScheduleParser->createSchedule();

        fixedRateBond = shared_ptr<FixedRateBond>(new FixedRateBond(
                this->settlementDays,
                this->faceAmount,
                *schedule,
                std::vector<Rate>(1, this->rate),
                this->dayCounter,
                this->convention,
                this->redemption,
                this->issueDate));

        fixedRateBond->setPricingEngine(bondEngine);

        return fixedRateBond;
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real fixedRateBondParser::getNPV(){
    try{
        return fixedRateBond->NPV();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real fixedRateBondParser::getCleanPrice(){
    try{
        return fixedRateBond->cleanPrice();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real fixedRateBondParser::getDirtyPrice(){
    try{
        return fixedRateBond->dirtyPrice();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real fixedRateBondParser::getAccruedAmount(){
    try{
        return fixedRateBond->accruedAmount();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real fixedRateBondParser::getYield(){
    try{
        return fixedRateBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

int fixedRateBondParser::getAccruedDays(){
    try{
        return BondFunctions::accruedDays(*fixedRateBond);
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Time fixedRateBondParser::getMacaulayDuration(){
    try{
        InterestRate yield(fixedRateBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency()), this->YieldParser->getDayCounter(),
                           this->YieldParser->getCompounding(), this->YieldParser->getFrequency());
        return BondFunctions::duration(*fixedRateBond, yield,
                                       Duration::Macaulay, Settings::instance().evaluationDate());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Time fixedRateBondParser::getModifiedDuration(){
    try{
        InterestRate yield(fixedRateBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency()), this->YieldParser->getDayCounter(),
                           this->YieldParser->getCompounding(), this->YieldParser->getFrequency());
        return BondFunctions::duration(*fixedRateBond, yield,
                                       Duration::Modified, Settings::instance().evaluationDate());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real fixedRateBondParser::getConvexity(){
    try{
        InterestRate yield(fixedRateBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency()), this->YieldParser->getDayCounter(),
                           this->YieldParser->getCompounding(), this->YieldParser->getFrequency());
        return BondFunctions::convexity(*fixedRateBond, yield, Settings::instance().evaluationDate());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real fixedRateBondParser::getBPS(){
    try{
        return BondFunctions::bps(*fixedRateBond, *this->discountingTermStructure, Settings::instance().evaluationDate());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

void fixedRateBondParser::getFlows(boost::shared_ptr<bondPrice> bondPrice){
    try{

        const Leg& cashflows = fixedRateBond->cashflows();

        for (Size i=0; i<cashflows.size(); ++i) {
            string s = typeid(cashflows[i]).name();

            boost::shared_ptr<FixedRateCoupon> coupon =
                    boost::dynamic_pointer_cast<FixedRateCoupon>(cashflows[i]);

            if (coupon) {

                if (!coupon->hasOccurred(Settings::instance().evaluationDate())) {

                    boost::shared_ptr<flowResult> result{new flowResult};

                    result->amount = coupon->amount();
                    result->accrualStartDate = coupon->accrualStartDate();
                    result->accrualEndDate = coupon->accrualEndDate();
                    result->fixing = coupon->rate();
                    result->discount = this->termStructure.currentLink()->discount(coupon->date());
                    result->price = coupon->amount() * this->termStructure.currentLink()->discount(coupon->date());
                    result->flowType = Interest;

                    bondPrice->flows.push_back(std::move(result));
                }else{
                    boost::shared_ptr<flowResult> result{new flowResult};

                    result->amount = coupon->amount();
                    result->accrualStartDate = coupon->accrualStartDate();
                    result->accrualEndDate = coupon->accrualEndDate();
                    result->fixing = coupon->rate();
                    result->flowType = PastInterest;

                    bondPrice->flows.push_back(std::move(result));
                }
            }else{
                boost::shared_ptr<CashFlow> coupon =
                        boost::dynamic_pointer_cast<CashFlow>(cashflows[i]);

                if (!coupon->hasOccurred(Settings::instance().evaluationDate())) {

                    boost::shared_ptr<flowResult> result {new flowResult};

                    result->date = coupon->date();
                    result->amount = coupon->amount();
                    result->discount = this->termStructure.currentLink()->discount(coupon->date());
                    result->price = coupon->amount() * this->termStructure.currentLink()->discount(coupon->date());
                    result->flowType = Notional;

                    bondPrice->flows.push_back(std::move(result));
                }

            }
        }
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}