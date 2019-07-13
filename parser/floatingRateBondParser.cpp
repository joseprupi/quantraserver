//
// Created by Josep Rubio on 6/14/17.
//

#include "floatingRateBondParser.h"

floatingRateBondParser::floatingRateBondParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;

}

void floatingRateBondParser::setSettlementDays(Integer settlementDays) {
    this->settlementDays = settlementDays;
}

void floatingRateBondParser::setFaceAmount(Real faceAmount) {
    this->faceAmount = faceAmount;
}

void floatingRateBondParser::setDayCounter(const DayCounter &dayCounter) {
    this->dayCounter = dayCounter;
}

void floatingRateBondParser::setConvention(const BusinessDayConvention &convention) {
    this->convention = convention;
}

void floatingRateBondParser::setFixingDays(int fixingDays) {
    this->fixingDays = fixingDays;
}

void floatingRateBondParser::setSpread(Rate spread) {
    this->spread = spread;
}

void floatingRateBondParser::setRedemption(Real redemption) {
    this->redemption = redemption;
}

void floatingRateBondParser::setIssueDate(const Date &issueDate) {
    this->issueDate = issueDate;
}

void floatingRateBondParser::setDiscountingTermStructure(boost::shared_ptr<YieldTermStructure> discountingTermStructure){
    this->discountingTermStructure = discountingTermStructure;
}

void floatingRateBondParser::setForecastingTermStructure(boost::shared_ptr<YieldTermStructure> forecastingTermStructure){
    this->forecastingTermStructure = forecastingTermStructure;
}

string floatingRateBondParser::getDiscountingTermStructureName(){
    return discountingTermStructureName;
}

string floatingRateBondParser::getForecastingTermStructureName(){
    return forecastingTermStructureName;
}

bool floatingRateBondParser::parse(Value &JSON){

    bool error = false;

    ostringstream erroros;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Index");
    if(iterator != JSON.MemberEnd()){
        Value& indexJSON = JSON["Index"];
        this->IndexParser = boost::shared_ptr<indexParser>(new indexParser(this->getErrorLog(), "FloatingRateBond Index"));
        error = error || IndexParser->parse(indexJSON);
    }else{
        erroros << "FloatingRateBond Index: Not found ";
        CROW_LOG_DEBUG << "FloatingRateBond Index: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    iterator = JSON.FindMember("Schedule");
    if(iterator != JSON.MemberEnd()){
        Value& scheduleJSON = JSON["Schedule"];
        this->ScheduleParser = boost::shared_ptr<scheduleParser>(new scheduleParser(this->getErrorLog(), "FloatingRateBond Schedule", false));
        error = error || ScheduleParser->parse(scheduleJSON);
    }else{
        erroros << "FloatingRateBond Schedule: Not found ";
        CROW_LOG_DEBUG << "FloatingRateBond Schedule: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    if(this->qGetString(JSON, "DiscountingTermStructure", &discountingTermStructureName, true)){
        error = true;
    }

    if(this->qGetString(JSON, "ForecastingTermStructure", &forecastingTermStructureName, true)){
        error = true;
    }

    if(this->qGetInt(JSON, "SettlementDays", &settlementDays, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "FaceAmount", &faceAmount, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "Spread", &spread, true)){
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
        this->YieldParser = boost::shared_ptr<yieldParser>(new yieldParser(this->getErrorLog(), "FloatingRateBond Yield"));
        error = error || YieldParser->parse(yieldJSON);
    }else{
        erroros << "FloatingRateBond Yield: Not found ";
        CROW_LOG_DEBUG << "FloatingRateBond Yield: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    return error;
}

shared_ptr<FloatingRateBond> floatingRateBondParser::createFloatingRateBond(){

    try {

        termStructure.linkTo(this->discountingTermStructure);

        boost::shared_ptr<PricingEngine> bondEngine(
                new DiscountingBondEngine(termStructure));

        this->IndexParser->setForecastingTermStructure(this->forecastingTermStructure);
        this->IndexParser->setPeriod(Period(this->ScheduleParser->getFrequency()));
        boost::shared_ptr<IborIndex> index =  this->IndexParser->createIndex();

        boost::shared_ptr<Schedule> schedule = this->ScheduleParser->createSchedule();

        floatingRateBond = shared_ptr<FloatingRateBond>(new FloatingRateBond(
                this->settlementDays,
                this->faceAmount,
                *schedule,
                index,
                this->dayCounter,
                this->convention,
                Natural(this->settlementDays),
                // Gearings
                std::vector<Real>(1, 1.0),
                // Spreads
                std::vector<Rate>(1, this->spread),
                // Caps
                std::vector<Rate>(),
                // Floors
                std::vector<Rate>(),
                // Fixing in arrears
                false,
                this->redemption,
                this->issueDate));

        floatingRateBond->setPricingEngine(bondEngine);

        return floatingRateBond;

    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real floatingRateBondParser::getNPV(){
    try{
        return floatingRateBond->NPV();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real floatingRateBondParser::getCleanPrice(){
    try{
        return floatingRateBond->cleanPrice();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real floatingRateBondParser::getDirtyPrice(){
    try{
        return floatingRateBond->dirtyPrice();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real floatingRateBondParser::getAccruedAmount(){
    try{
        return floatingRateBond->accruedAmount();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real floatingRateBondParser::getYield(){
    try{
        return floatingRateBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

int floatingRateBondParser::getAccruedDays(){
    try{
        return BondFunctions::accruedDays(*floatingRateBond);
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Time floatingRateBondParser::getMacaulayDuration(){
    try{
        InterestRate yield(floatingRateBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency()), this->YieldParser->getDayCounter(),
                           this->YieldParser->getCompounding(), this->YieldParser->getFrequency());
        return BondFunctions::duration(*floatingRateBond, yield,
                                       Duration::Macaulay, Settings::instance().evaluationDate());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Time floatingRateBondParser::getModifiedDuration(){
    try{
        InterestRate yield(floatingRateBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency()), this->YieldParser->getDayCounter(),
                           this->YieldParser->getCompounding(), this->YieldParser->getFrequency());
        return BondFunctions::duration(*floatingRateBond, yield,
                                       Duration::Modified, Settings::instance().evaluationDate());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real floatingRateBondParser::getConvexity(){
    try{
        InterestRate yield(floatingRateBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency()), this->YieldParser->getDayCounter(),
                           this->YieldParser->getCompounding(), this->YieldParser->getFrequency());
        return BondFunctions::convexity(*floatingRateBond, yield, Settings::instance().evaluationDate());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real floatingRateBondParser::getBPS(){
    try{
        return BondFunctions::bps(*floatingRateBond, *this->discountingTermStructure, Settings::instance().evaluationDate());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

void floatingRateBondParser::getFlows(boost::shared_ptr<bondPrice> bondPrice){
    try{

        const Leg& cashflows = floatingRateBond->cashflows();

        for (Size i=0; i<cashflows.size(); ++i) {
            string s = typeid(cashflows[i]).name();

            boost::shared_ptr<FloatingRateCoupon> coupon =
                    boost::dynamic_pointer_cast<FloatingRateCoupon>(cashflows[i]);

            if (coupon) {

                if (!coupon->hasOccurred(Settings::instance().evaluationDate())) {

                    boost::shared_ptr<flowResult> result{new flowResult};

                    result->date = coupon->fixingDate();
                    result->amount = coupon->amount();
                    result->accrualStartDate = coupon->accrualStartDate();
                    result->accrualEndDate = coupon->accrualEndDate();
                    result->fixing = coupon->indexFixing();
                    result->discount = this->termStructure.currentLink()->discount(coupon->date());
                    result->price = coupon->amount() * this->termStructure.currentLink()->discount(coupon->date());
                    result->flowType = Interest;

                    bondPrice->flows.push_back(std::move(result));
                }else{
                    boost::shared_ptr<flowResult> result{new flowResult};

                    result->date = coupon->fixingDate();
                    result->amount = coupon->amount();
                    result->accrualStartDate = coupon->accrualStartDate();
                    result->accrualEndDate = coupon->accrualEndDate();
                    result->fixing = coupon->indexFixing();
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