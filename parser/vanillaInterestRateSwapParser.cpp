//
// Created by Josep Rubio on 7/4/17.
//

#include "vanillaInterestRateSwapParser.h"

vanillaInterestRateSwapParser::vanillaInterestRateSwapParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;

}

void vanillaInterestRateSwapParser::setDiscountingTermStructure(boost::shared_ptr<YieldTermStructure> discountingTermStructure){
    this->discountingTermStructure = discountingTermStructure;
}

void vanillaInterestRateSwapParser::setForecastingTermStructure(boost::shared_ptr<YieldTermStructure> forecastingTermStructure){
    this->forecastingTermStructure = forecastingTermStructure;
}

string vanillaInterestRateSwapParser::getDiscountingTermStructureName(){
    return discountingTermStructureName;
}

string vanillaInterestRateSwapParser::getForecastingTermStructureName(){
    return forecastingTermStructureName;
}

bool vanillaInterestRateSwapParser::parse(Value &JSON) {

    bool error = false;
    ostringstream erroros;

    if(this->qGetDouble(JSON, "Notional", &nominal, true)){
        error = true;
    }

    if(this->qGetSwapType(JSON, "SwapType", &swapType, true)){
        error = true;
    }

    bool swapClendar = false;
    bool swapStartDate = false;
    bool swapLengthInYears = false;

    Value::MemberIterator iterator;

    if(this->qGetString(JSON, "DiscountingTermStructure", &discountingTermStructureName, true)){
        error = true;
    }

    if(this->qGetString(JSON, "ForecastingTermStructure", &forecastingTermStructureName, true)){
        error = true;
    }

    iterator = JSON.FindMember("Calendar");
    if(iterator != JSON.MemberEnd()){
        swapClendar = true;
    }

    iterator = JSON.FindMember("StartDate");
    if(iterator != JSON.MemberEnd()){
        swapStartDate = true;
    }

    iterator = JSON.FindMember("LengthInYears");
    if(iterator != JSON.MemberEnd()){
        swapLengthInYears = true;
    }

    if(swapClendar == false && swapStartDate == false && swapLengthInYears  == false) {
        genericSchedule = false;
    }else if(swapClendar == true && swapStartDate == true && swapLengthInYears  == true) {
        if(this->qGetCalendar(JSON, "Calendar", &calendar, true)){
            error = true;
        }

        if(this->qGetDate(JSON, "StartDate", &startDate, true, calendar)){
            error = true;
        }

        if(this->qGetInt(JSON, "LengthInYears", &lengthInYears, true)){
            error = true;
        }
        genericSchedule = true;
    }
    else{
        erroros  << message << ": Calendar, StartDate and LengthInYears have to be provided to override Swap schedule dates";
        CROW_LOG_DEBUG  << message << ": Calendar, StartDate and LengthInYears have to be provided to override Swap schedule dates";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    iterator = JSON.FindMember("FixedLeg");
    if(iterator != JSON.MemberEnd()){
        Value& fixedLegJSON = JSON["FixedLeg"];
        this->FixedLegParser = boost::shared_ptr<fixedLegParser>(new fixedLegParser(this->getErrorLog(), "Swap FixedLeg", genericSchedule));
        error = error || this->FixedLegParser->parse(fixedLegJSON);
    }else{
        erroros << "Swap FixedLeg: Not found ";
        CROW_LOG_DEBUG << "Swap FixedLeg: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    iterator = JSON.FindMember("FloatingLeg");
    if(iterator != JSON.MemberEnd()){
        Value& floatingLegJSON = JSON["FloatingLeg"];
        this->FloatingLegParser = boost::shared_ptr<floatingLegParser>(new floatingLegParser(this->getErrorLog(), "Swap FloatingLeg", genericSchedule));
        error = error || this->FloatingLegParser->parse(floatingLegJSON);
    }else{
        erroros << "Swap FloatingLeg: Not found ";
        CROW_LOG_DEBUG << "Swap FloatingLeg: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    return error;

}

shared_ptr<VanillaSwap> vanillaInterestRateSwapParser::createVanillaSwap(){

    termStructure.linkTo(this->discountingTermStructure);

    if(genericSchedule){
        Date maturity = startDate + lengthInYears * Years;
        this->FixedLegParser->getSchedule()->setCalendar(calendar);
        this->FloatingLegParser->getSchedule()->setCalendar(calendar);
        this->FixedLegParser->getSchedule()->setEffectiveDate(startDate);
        this->FloatingLegParser->getSchedule()->setEffectiveDate(startDate);
        this->FixedLegParser->getSchedule()->setTerminationDate(maturity);
        this->FloatingLegParser->getSchedule()->setTerminationDate(maturity);
    }

    boost::shared_ptr<Schedule> fixedSchedule = this->FixedLegParser->getSchedule()->createSchedule();
    boost::shared_ptr<Schedule> floatingSchedule = this->FloatingLegParser->getSchedule()->createSchedule();

    this->FloatingLegParser->getIndexParser()->setForecastingTermStructure(this->forecastingTermStructure);
    this->FloatingLegParser->getIndexParser()->setPeriod(Period(this->FloatingLegParser->getSchedule()->getFrequency()));
    boost::shared_ptr<IborIndex> index =  this->FloatingLegParser->getIndexParser()->createIndex();

    vanillaSwap = shared_ptr<VanillaSwap>(new VanillaSwap(swapType, nominal,
                                                        *fixedSchedule, this->FixedLegParser->getRate(),
                                                          this->FixedLegParser->getDayCounter(),
                                                          *floatingSchedule, index, this->FloatingLegParser->getSpread(),
                                                          this->FloatingLegParser->getDayCounter()));

    boost::shared_ptr<PricingEngine> swapEngine(
            new DiscountingSwapEngine(this->termStructure));
    vanillaSwap->setPricingEngine(swapEngine);

    return vanillaSwap;

}

Real vanillaInterestRateSwapParser::getNPV(){
    try{
        return vanillaSwap->NPV();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Rate vanillaInterestRateSwapParser::getFairRate(){
    try{
        return vanillaSwap->fairRate();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

Spread vanillaInterestRateSwapParser::getFairSpread(){
    try{
        return vanillaSwap->fairSpread();
    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }
}

void vanillaInterestRateSwapParser::getFlows(boost::shared_ptr<swapPrice> swapPrice){
    try{

        const Leg& fixedCoupons = vanillaSwap->fixedLeg();

        for (Size i=0; i<fixedCoupons.size(); ++i) {
            boost::shared_ptr<FixedRateCoupon> coupon =
                    boost::dynamic_pointer_cast<FixedRateCoupon>(fixedCoupons[i]);

            if (!coupon->hasOccurred(Settings::instance().evaluationDate())) {

                boost::shared_ptr<flowResult> result{new flowResult};

                result->amount = coupon->amount();
                result->accrualStartDate = coupon->accrualStartDate();
                result->accrualEndDate = coupon->accrualEndDate();
                result->fixing = coupon->rate();
                result->discount = this->termStructure.currentLink()->discount(coupon->date());
                result->price = coupon->amount() * this->termStructure.currentLink()->discount(coupon->date());
                result->flowType = Interest;

                swapPrice->fixedFlows.push_back(std::move(result));
            }else{
                boost::shared_ptr<flowResult> result{new flowResult};

                result->amount = coupon->amount();
                result->accrualStartDate = coupon->accrualStartDate();
                result->accrualEndDate = coupon->accrualEndDate();
                result->fixing = coupon->rate();
                result->flowType = PastInterest;

                swapPrice->fixedFlows.push_back(std::move(result));
            }
        }

        const Leg& floatingCoupons = vanillaSwap->floatingLeg();

        for (Size i=0; i<floatingCoupons.size(); ++i) {
            boost::shared_ptr<FloatingRateCoupon> coupon =
                    boost::dynamic_pointer_cast<FloatingRateCoupon>(floatingCoupons[i]);

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

                swapPrice->floatingFlows.push_back(std::move(result));
            }else{
                boost::shared_ptr<flowResult> result{new flowResult};

                result->date = coupon->fixingDate();
                result->amount = coupon->amount();
                result->accrualStartDate = coupon->accrualStartDate();
                result->accrualEndDate = coupon->accrualEndDate();
                result->fixing = coupon->indexFixing();
                result->flowType = PastInterest;

                swapPrice->floatingFlows.push_back(std::move(result));
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