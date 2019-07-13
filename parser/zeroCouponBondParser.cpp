//
// Created by Josep Rubio on 6/27/17.
//

#include "zeroCouponBondParser.h"

zeroCouponBondParser::zeroCouponBondParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;

}

void zeroCouponBondParser::setSettlementDays(Integer settlementDays){
    this->settlementDays = settlementDays;
}

void zeroCouponBondParser::setCalendar(Calendar calendar){
    this->calendar = calendar;
}

Calendar zeroCouponBondParser::getCalendar(){
    return this->calendar;
}

void zeroCouponBondParser::setFaceAmount(Real faceAmount){
    this->faceAmount = faceAmount;
}

void zeroCouponBondParser::setMaturityDate(Date maturityDate){
    this->maturityDate = maturityDate;
}

void zeroCouponBondParser::setPaymentConvention(BusinessDayConvention paymentConvention){
    this->paymentConvention = paymentConvention;
}

void zeroCouponBondParser::setRedemption(Real redemption){
    this->redemption = redemption;
}

void zeroCouponBondParser::setIssueDate(Date issueDate){
    this->issueDate = issueDate;
}

void zeroCouponBondParser::setDiscountingTermStructure(boost::shared_ptr<YieldTermStructure> termStructure) {
    this->discountingTermStructure.linkTo(termStructure);
}

string zeroCouponBondParser::getDiscountingTermStructureName(){
    return discountingTermStructureName;
}

bool zeroCouponBondParser::parse(Value &JSON){

    bool error = false;
    ostringstream erroros;

    if(this->qGetString(JSON, "DiscountingTermStructure", &discountingTermStructureName, true)){
        error = true;
    }

    if(this->qGetCalendar(JSON, "Calendar", &calendar, true)){
        error = true;
    }

    if(this->qGetInt(JSON, "SettlementDays", &settlementDays, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "FaceAmount", &faceAmount, true)){
        error = true;
    }

    if(this->qGetConvention(JSON, "Convention", &paymentConvention, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "Redemption", &redemption, true)){
        error = true;
    }

    if(this->qGetDate(JSON, "IssueDate", &issueDate, true, calendar)){
        error = true;
    }

    if(this->qGetDate(JSON, "MaturityDate", &maturityDate, true, calendar)){
        error = true;
    }

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Yield");
    if(iterator != JSON.MemberEnd()){
        Value& yieldJSON = JSON["Yield"];
        this->YieldParser = boost::shared_ptr<yieldParser>(new yieldParser(this->getErrorLog(), "ZeroCouponBond Yield"));
        error = error || YieldParser->parse(yieldJSON);
    }else{
        erroros << "ZeroCouponBond Yield: Not found ";
        CROW_LOG_DEBUG << "ZeroCouponBond Yield: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    return error;
}

shared_ptr<ZeroCouponBond> zeroCouponBondParser::createZeroCouponBond(){

    try {
        boost::shared_ptr<PricingEngine> bondEngine(
                new DiscountingBondEngine(this->discountingTermStructure));
        // Zero coupon bond

        zeroCouponBond = shared_ptr<ZeroCouponBond>(new ZeroCouponBond(
                this->settlementDays,
                this->calendar,
                this->faceAmount,
                this->maturityDate,
                this->paymentConvention,
                this->redemption,
                this->issueDate));

        zeroCouponBond->setPricingEngine(bondEngine);

        return zeroCouponBond;
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real zeroCouponBondParser::getNPV(){
    try{
        return zeroCouponBond->NPV();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real zeroCouponBondParser::getCleanPrice(){
    try{
        return zeroCouponBond->cleanPrice();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real zeroCouponBondParser::getDirtyPrice(){
    try{
        return zeroCouponBond->dirtyPrice();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real zeroCouponBondParser::getAccruedAmount(){
    try{
        return zeroCouponBond->accruedAmount();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Real zeroCouponBondParser::getYield(){
    try{
        return zeroCouponBond->yield(this->YieldParser->getDayCounter(),this->YieldParser->getCompounding(),this->YieldParser->getFrequency());
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

void zeroCouponBondParser::getFlows(boost::shared_ptr<bondPrice> price){
    try{
        const Leg& cashflows = zeroCouponBond->cashflows();

        for (Size i=0; i<cashflows.size(); ++i) {
            boost::shared_ptr<SimpleCashFlow> coupon =
                    boost::dynamic_pointer_cast<SimpleCashFlow>(cashflows[i]);

            if (!coupon->hasOccurred(Settings::instance().evaluationDate())) {

                boost::shared_ptr<flowResult> result {new flowResult};

                result->date = coupon->date();
                result->amount = coupon->amount();
                result->discount = this->discountingTermStructure.currentLink()->discount(coupon->date());
                result->price = coupon->amount() * this->discountingTermStructure.currentLink()->discount(coupon->date());

                price->flows.push_back(std::move(result));
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