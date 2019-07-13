//
// Created by Josep Rubio on 6/14/17.
//

#include "termStructureParser.h"

termStructureParser::termStructureParser(std::string message, std::vector<std::string> *errorLog,
                                         std::map<string,boost::shared_ptr<termStructureParser>> termStructuresList)
        :QuantraParser(message, errorLog){

    this->termStructuresList = termStructuresList;
    this->message = message;

}

void termStructureParser::setInterpolator(const InterpolationEnum &interpolator) {
        this->interpolator = interpolator;
}

void termStructureParser::setDayCounter(const DayCounter &dayCounter) {
    this->dayCounter = dayCounter;
}

void termStructureParser::setAsOfDate(const Date &asOfDate) {
    this->asOfDate = asOfDate;
}

DayCounter termStructureParser::getDayCounter(){
    return this->dayCounter;
}

string termStructureParser::getId(){
    return this->id;
}

bool termStructureParser::SetPoint(boost::shared_ptr<termStructurePointParser> rp){

    try {

        boost::shared_ptr<RateHelper> rateHelper;

        switch (rp->getType()) {
            case DepositString: {

                boost::shared_ptr<Quote> quote(new SimpleQuote(rp->getRate()));

                rateHelper = boost::shared_ptr<RateHelper>(new DepositRateHelper(
                        Handle<Quote>(quote),
                        rp->getTenorNumber() * rp->getTenorTimeUnit(),
                        rp->getFixingDays(),
                        rp->getCalendar(),
                        rp->getConvention(),
                        true,
                        rp->getDayCounter()));
                break;
            }

            case FraString: {

                boost::shared_ptr<Quote> quote(new SimpleQuote(rp->getRate()));

                rateHelper = boost::shared_ptr<RateHelper>(new FraRateHelper(
                        Handle<Quote>(quote),
                        rp->getMonthsToStart(),
                        rp->getMonthsToEnd(),
                        rp->getFixingDays(),
                        rp->getCalendar(),
                        rp->getConvention(),
                        true,
                        rp->getDayCounter()));
                break;
            }

            case FutureString: {

                boost::shared_ptr<Quote> quote(new SimpleQuote(rp->getRate()));

                rateHelper = boost::shared_ptr<RateHelper>(new FuturesRateHelper(
                        Handle<Quote>(quote),
                        rp->getFutureStartDate(),
                        rp->getFutMonths(),
                        rp->getCalendar(),
                        rp->getConvention(),
                        true,
                        rp->getDayCounter()));
                break;
            }

            case SwapString: {

                boost::shared_ptr<Quote> quote(new SimpleQuote(rp->getRate()));

                if(rp->getExogeneousPoint()){
                    rateHelper = boost::shared_ptr<RateHelper>(new SwapRateHelper(
                            Handle<Quote>(quote),
                            rp->getTenorNumber() * rp->getTenorTimeUnit(),
                            rp->getCalendar(),
                            rp->getSwFixedLegFrequency(),
                            rp->getSwFixedLegConvention(),
                            rp->getSwFixedLegDayCounter(),
                            rp->getIndex(),
                            Handle<Quote>(),
                            Period(0*Days),
                            Handle<YieldTermStructure>(rp->getTermStructure())));
                }else{
                    rateHelper = boost::shared_ptr<RateHelper>(new SwapRateHelper(
                            Handle<Quote>(quote),
                            rp->getTenorNumber() * rp->getTenorTimeUnit(),
                            rp->getCalendar(),
                            rp->getSwFixedLegFrequency(),
                            rp->getSwFixedLegConvention(),
                            rp->getSwFixedLegDayCounter(),
                            rp->getIndex()));
                }



                break;
            }

            case BondString: {

                boost::shared_ptr<Schedule> schedule = rp->getScheduleParser()->createSchedule();

                boost::shared_ptr<Quote> quote(new SimpleQuote(rp->getRate()));

                rateHelper = boost::shared_ptr<RateHelper>(new FixedRateBondHelper(
                        Handle<Quote>(quote),
                        rp->getFixingDays(),
                        rp->getFaceAmount(),
                        *schedule,
                        std::vector<Rate>(1, rp->getCouponRate()),
                        rp->getDayCounter(),
                        rp->getConvention(),
                        rp->getRedemption(),
                        rp->getIssueDate()));

                break;
            }

            case OISString: {

                boost::shared_ptr<Quote> quote(new SimpleQuote(rp->getRate()));

                rateHelper = boost::shared_ptr<RateHelper>(new OISRateHelper(
                        rp->getFixingDays(),
                        rp->getTenorNumber() * rp->getTenorTimeUnit(),
                        Handle<Quote>(quote),
                        rp->getOvernightIndex()
                ));

                break;
            }

            case DatedOISString: {

                boost::shared_ptr<Quote> quote(new SimpleQuote(rp->getRate()));

                rateHelper = boost::shared_ptr<RateHelper>(new DatedOISRateHelper(
                        rp->getStartDate(),
                        rp->getEndtDate(),
                        Handle<Quote>(quote),
                        rp->getOvernightIndex()
                ));

                break;
            }
        }

        rateHelperStructure.push_back(rateHelper);

    }catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        //throw std::runtime_error(s);
        QL_FAIL(s);
    }

    return true;
}

bool termStructureParser::parse(Value &JSON){

    std::ostringstream erroros;
    bool error = false;
    bool pointError = false;
    bool jumpError = false;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember("Id");
    if (iterator != JSON.MemberEnd()) {
        if(this->qGetString(JSON, "Id", &id, true)){
            error = true;
        }
    } else {
        erroros << "TermStructure Id: Not found ";
        CROW_LOG_DEBUG << "TermStructure Id: Not found ";
        this->getErrorLog()->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    iterator = JSON.FindMember("TermStructure");
    if (iterator != JSON.MemberEnd()) {

        if(this->qGetBootstrapTrait(JSON["TermStructure"], "BootstrapTrait", &bootstrapTrait, true)){
            error = true;
        }

        if(bootstrapTrait == InterpolatedFwdString){

            this->interpolated = true;

        }

        if(this->qGetDayCounter(JSON["TermStructure"], "DayCounter", &dayCounter, true)){
            error = true;
        }

        if(this->qInterpolator(JSON["TermStructure"], "Interpolator", &interpolator, true)){
            error = true;
        }

        iterator = JSON["TermStructure"].FindMember("ZeroSpread");
        if (iterator != JSON["TermStructure"].MemberEnd()) {
            if (JSON["TermStructure"]["ZeroSpread"].IsObject()) {
                Value& zeroSpreadJSON = JSON["TermStructure"]["ZeroSpread"];
                this->ZeroCurveSpreadParser =
                        boost::shared_ptr<zeroCurveSpreadParser>(
                                new zeroCurveSpreadParser(this->getErrorLog(), "Term Structure Zero Spread"));
                error = error || this->ZeroCurveSpreadParser->parse(zeroSpreadJSON);
                if(!error){
                    if(this->ZeroCurveSpreadParser->getZeroSpreadType() == ParallelString){
                        hasSimpleSpread = true;
                    }else if(this->ZeroCurveSpreadParser->getZeroSpreadType() == LinearInterpolatedString){
                        hasInterpolatedSpread = true;
                    }
                }
            } else {
                erroros << "ZeroSpread has to be an object";
                CROW_LOG_DEBUG << "ZeroSpread has to be an object";
                this->getErrorLog()->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }
        }


    }else if((iterator = JSON.FindMember("FlatTermStructure")) != JSON.MemberEnd()) {

        if(this->qGetRawDate(JSON["FlatTermStructure"], "Date", &flatDate, true)){
            error = true;
        }

        if(this->qGetDouble(JSON["FlatTermStructure"], "Rate", &flatRate, true)){
            error = true;
        }

        if(this->qGetDayCounter(JSON["FlatTermStructure"], "DayCounter", &dayCounter, true)){
            error = true;
        }

        this->isFlat = true;

    }else {
        erroros << "TermStructure or FlatTermStructure: Not found ";
        CROW_LOG_DEBUG << "TermStructure or FlatTermStructure: Not found ";
        this->getErrorLog()->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    if(!this->isFlat) {

        iterator = JSON.FindMember("Points");
        if (iterator != JSON.MemberEnd()) {

            if (!JSON["Points"].IsArray() || JSON["Points"].GetArray().Size() == 0) {
                CROW_LOG_DEBUG << "Points has to be an array with at least 1 element";
                erroros << "Points has to be an array with at least 1 element";
                this->getErrorLog()->push_back(erroros.str());
                error = true;
            } else {
                int i = 0;

                while (i < JSON["Points"].Size()) {

                    if (interpolated) {
                        Date date;
                        Rate rate;

                        bool tempError = false;

                        if (this->qGetDouble(JSON["Points"][i], "Rate", &rate, true)) {
                            tempError = true;
                        }

                        if (this->qGetRawDate(JSON["Points"][i], "Date", &date, true)) {
                            tempError = true;
                        }
                        if (!tempError) {
                            this->interpolatedDates.push_back(date);
                            this->interpolatedRates.push_back(rate);
                        }

                    } else {
                        boost::shared_ptr<termStructurePointParser>
                                TermStructurePointParser(
                                new termStructurePointParser(this->getMessage(), this->getErrorLog(),
                                                             this->termStructuresList));
                        TermStructurePointParser->setMessage(this->getMessage() + " Point: " + std::to_string(i) + " ");
                        pointError = TermStructurePointParser->parse(JSON["Points"][i]);

                        if (pointError == false) {
                            try {
                                this->exogeneousDiscount =
                                        this->exogeneousDiscount || TermStructurePointParser->getExogeneousPoint();
                                this->SetPoint(TermStructurePointParser);
                            } catch (Error re) {
                                pointError = true;
                                erroros << "Error adding point: " << i << " ." << re.what();
                                CROW_LOG_DEBUG << "Error adding point: " << i << " ." << re.what();
                                this->getErrorLog()->push_back(erroros.str());
                                erroros.str("");
                                erroros.clear();
                            }

                        } else {
                            error = true;
                        }
                    }

                    i++;
                }
            }

        } else {
            erroros << "Points: Not found ";
            CROW_LOG_DEBUG << "Points: Not found ";
            this->getErrorLog()->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }

        iterator = JSON.FindMember("Jumps");
        if (iterator != JSON.MemberEnd()) {

            if (!JSON["Jumps"].IsArray() || JSON["Jumps"].GetArray().Size() == 0) {
                CROW_LOG_DEBUG << "Jumps has to be an array with at least 1 element";
                erroros << "Jumps has to be an array with at least 1 element";
                this->getErrorLog()->push_back(erroros.str());
                error = true;
            } else {
                int i = 0;

                while (i < JSON["Jumps"].Size()) {

                    Date jumpDate;
                    Rate jumpRate;

                    if (this->qGetRawDate(JSON["Jumps"][i], "Date", &jumpDate, true)) {
                        jumpError = true;
                    }

                    if (this->qGetDouble(JSON["Jumps"][i], "Rate", &jumpRate, true)) {
                        error = true;
                    }

                    if (jumpError == false) {
                        try {
                            boost::shared_ptr<Quote> quote(new SimpleQuote(jumpRate));
                            this->jumps.push_back(Handle<Quote>(quote));
                            this->jumpDates.push_back(jumpDate);
                        } catch (Error re) {
                            error = true;
                            erroros << "Error adding jump: " << i << " ." << re.what();
                            CROW_LOG_DEBUG << "Error adding jump: " << i << " ." << re.what();
                            this->getErrorLog()->push_back(erroros.str());
                            erroros.str("");
                            erroros.clear();
                        }

                    } else {
                        error = true;
                    }

                    i++;
                }
            }

        }
    }

    return error;
}

boost::shared_ptr<YieldTermStructure> termStructureParser::getTermStructure() {

    return tmpTermStructure;
}

boost::shared_ptr<YieldTermStructure> termStructureParser::createTermStructure() {

    try {

        Settings::instance().evaluationDate() = this->asOfDate;

        double tolerance = 1.0e-12;

        boost::shared_ptr<YieldTermStructure> termStructure;

        if(this->isFlat){
            termStructure = boost::shared_ptr<YieldTermStructure>(
                    new FlatForward(
                            this->flatDate,
                            this->flatRate,
                            this->dayCounter));
            tmpTermStructure = termStructure;
        }else {

            switch (this->interpolator) {

                case BackwardFlatString: {

                    switch (this->bootstrapTrait) {

                        case DiscountString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<Discount, BackwardFlat>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<Discount, BackwardFlat>>(
                                    termStructure)->nodes();

                            break;
                        }

                        case ZeroRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ZeroYield, BackwardFlat>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ZeroYield, BackwardFlat>>(
                                    termStructure)->nodes();

                            break;
                        }

                        case FwdRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ForwardRate, BackwardFlat>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ForwardRate, BackwardFlat>>(
                                    termStructure)->nodes();

                            break;
                        }

                        case InterpolatedFwdString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedForwardCurve<BackwardFlat>(this->interpolatedDates,
                                                                               this->interpolatedRates,
                                                                               this->dayCounter));
                            break;
                        }

                        case InterpolatedZeroString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedZeroCurve<BackwardFlat>(this->interpolatedDates,
                                                                            this->interpolatedRates,
                                                                            this->dayCounter));
                            break;
                        }

                        case InterpolatedDiscountString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedDiscountCurve<BackwardFlat>(this->interpolatedDates,
                                                                                this->interpolatedRates,
                                                                                this->dayCounter));
                            break;
                        }
                    }


                    break;
                }

                case ForwardFlatString: {

                    switch (this->bootstrapTrait) {

                        case DiscountString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<Discount, ForwardFlat>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<Discount, ForwardFlat>>(
                                    termStructure)->nodes();

                            break;
                        }

                        case ZeroRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ZeroYield, ForwardFlat>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ZeroYield, ForwardFlat>>(
                                    termStructure)->nodes();

                            break;
                        }

                        case FwdRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ForwardRate, ForwardFlat>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ForwardRate, ForwardFlat>>(
                                    termStructure)->nodes();

                            break;

                        }

                        case InterpolatedFwdString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedForwardCurve<ForwardFlat>(this->interpolatedDates,
                                                                              this->interpolatedRates,
                                                                              this->dayCounter));
                            break;
                        }

                        case InterpolatedZeroString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedZeroCurve<ForwardFlat>(this->interpolatedDates,
                                                                           this->interpolatedRates,
                                                                           this->dayCounter));
                            break;
                        }

                        case InterpolatedDiscountString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedDiscountCurve<ForwardFlat>(this->interpolatedDates,
                                                                               this->interpolatedRates,
                                                                               this->dayCounter));
                            break;
                        }
                    }

                    break;

                }

                case LinearString: {

                    switch (this->bootstrapTrait) {

                        case DiscountString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<Discount, Linear>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<Discount, Linear>>(
                                    termStructure)->nodes();

                            break;
                        }

                        case ZeroRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ZeroYield, Linear>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ZeroYield, Linear>>(
                                    termStructure)->nodes();

                            break;
                        }

                        case FwdRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ForwardRate, Linear>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ForwardRate, Linear>>(
                                    termStructure)->nodes();

                            break;

                        }

                        case InterpolatedFwdString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedForwardCurve<Linear>(this->interpolatedDates,
                                                                         this->interpolatedRates,
                                                                         this->dayCounter));
                            break;
                        }

                        case InterpolatedZeroString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedZeroCurve<Linear>(this->interpolatedDates,
                                                                      this->interpolatedRates,
                                                                      this->dayCounter));
                            break;
                        }

                        case InterpolatedDiscountString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedDiscountCurve<Linear>(this->interpolatedDates,
                                                                          this->interpolatedRates,
                                                                          this->dayCounter));
                            break;
                        }

                    }


                    break;

                }

                case LogLinearString: {

                    switch (this->bootstrapTrait) {

                        case DiscountString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<Discount, LogLinear>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<Discount, LogLinear>>(
                                    termStructure)->nodes();

                            break;

                        }

                        case ZeroRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ZeroYield, LogLinear>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ZeroYield, LogLinear>>(
                                    termStructure)->nodes();

                            break;

                        }

                        case FwdRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ForwardRate, LogLinear>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ForwardRate, LogLinear>>(
                                    termStructure)->nodes();

                            break;

                        }

                        case InterpolatedFwdString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedForwardCurve<LogLinear>(this->interpolatedDates,
                                                                            this->interpolatedRates,
                                                                            this->dayCounter));
                            break;
                        }

                        case InterpolatedZeroString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedZeroCurve<LogLinear>(this->interpolatedDates,
                                                                         this->interpolatedRates,
                                                                         this->dayCounter));
                            break;
                        }

                        case InterpolatedDiscountString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedDiscountCurve<LogLinear>(this->interpolatedDates,
                                                                             this->interpolatedRates,
                                                                             this->dayCounter));
                            break;
                        }
                    }


                    break;

                }

                case LogCubicString: {

                    LogCubic mlcns = LogCubic(
                            CubicInterpolation::Spline,
                            true,
                            CubicInterpolation::SecondDerivative,
                            0,
                            CubicInterpolation::SecondDerivative,
                            0);

                    switch (this->bootstrapTrait) {

                        case DiscountString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<Discount, LogCubic>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance,
                                            mlcns));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<Discount, LogCubic>>(
                                    termStructure)->nodes();

                            break;

                        }

                        case ZeroRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ZeroYield, LogCubic>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance,
                                            mlcns));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ZeroYield, LogCubic>>(
                                    termStructure)->nodes();

                            break;

                        }

                        case FwdRateString: {

                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new PiecewiseYieldCurve<ForwardRate, LogCubic>(
                                            this->asOfDate,
                                            rateHelperStructure,
                                            this->dayCounter,
                                            jumps,
                                            jumpDates,
                                            tolerance,
                                            mlcns));

                            this->nodes = boost::dynamic_pointer_cast<PiecewiseYieldCurve<ForwardRate, LogCubic>>(
                                    termStructure)->nodes();

                            break;

                        }

                        case InterpolatedFwdString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedForwardCurve<LogCubic>(this->interpolatedDates,
                                                                           this->interpolatedRates,
                                                                           this->dayCounter,
                                                                           mlcns));
                            break;
                        }

                        case InterpolatedZeroString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedZeroCurve<LogCubic>(this->interpolatedDates,
                                                                        this->interpolatedRates,
                                                                        this->dayCounter,
                                                                        mlcns));
                            break;
                        }

                        case InterpolatedDiscountString: {
                            termStructure = boost::shared_ptr<YieldTermStructure>(
                                    new InterpolatedDiscountCurve<LogCubic>(this->interpolatedDates,
                                                                            this->interpolatedRates,
                                                                            this->dayCounter,
                                                                            mlcns));
                            break;
                        }
                    }

                    break;

                }

                default: {
                    throw "Interpolator not supported";

                }
            }

            if (hasSimpleSpread) {

                boost::shared_ptr<ZeroSpreadedTermStructure>
                        spreaded(new ZeroSpreadedTermStructure(Handle<YieldTermStructure>(termStructure),
                                                               Handle<Quote>(
                                                                       this->ZeroCurveSpreadParser->getSimpleSpread())));

                tmpTermStructure = spreaded;
            } else if (hasInterpolatedSpread) {

                boost::shared_ptr<PiecewiseZeroSpreadedTermStructure>
                        spreaded(new PiecewiseZeroSpreadedTermStructure(Handle<YieldTermStructure>(termStructure),
                                                                        this->ZeroCurveSpreadParser->getSpreadRates(),
                                                                        this->ZeroCurveSpreadParser->getSpreadDates()));

                tmpTermStructure = spreaded;
            } else {
                tmpTermStructure = termStructure;
            }
        }

        return termStructure;

    } catch (Error e) {
        std::stringstream errorString;
        errorString << e.what();
        std::string s = errorString.str();
        QL_FAIL(s);
    }

}