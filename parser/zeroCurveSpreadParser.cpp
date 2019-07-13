//
// Created by Josep Rubio on 3/7/18.
//

#include "zeroCurveSpreadParser.h"

zeroCurveSpreadParser::zeroCurveSpreadParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->errorLog = errorLog;

}


ZeroSpreadTypeEnum zeroCurveSpreadParser::getZeroSpreadType(){
    return this->zeroSpreadType;
}

boost::shared_ptr<Quote> zeroCurveSpreadParser::getSimpleSpread(){
    return this->simpleSpread;
}

std::vector<Handle<Quote>> zeroCurveSpreadParser::getSpreadRates(){
    return this->spreadRates;
}

std::vector<Date> zeroCurveSpreadParser::getSpreadDates(){
    return this->spreadDates;
}

bool zeroCurveSpreadParser::parse(Value &JSON) {

    bool error = false;

    ostringstream erroros;

    if(this->qGetZeroSpreadType(JSON, "ZeroSpreadType", &zeroSpreadType, true)){
        error = true;
    }

    if(!error) {
        if (zeroSpreadType == ParallelString) {
            double tmpRate;
            if(this->qGetDouble(JSON, "Spread", &tmpRate, true)){
                error = true;
            }
            simpleSpread = boost::shared_ptr<Quote>(new SimpleQuote(tmpRate));
        } else if (zeroSpreadType == LinearInterpolatedString) {

            Value::MemberIterator iterator;

            iterator = JSON.FindMember("Spreads");
            if (iterator != JSON.MemberEnd()) {

                if (!JSON["Spreads"].IsArray() || JSON["Spreads"].GetArray().Size() == 0){
                    CROW_LOG_DEBUG << "Spreads has to be an array with at least 1 element";
                    erroros << "Spreads has to be an array with at least 1 element";
                    this->getErrorLog()->push_back(erroros.str());
                    error = true;
                }else{
                    int i = 0;

                    while (i < JSON["Spreads"].Size()) {

                        Date date;
                        Rate rate;

                        bool tempError = false;

                        if(this->qGetDouble(JSON["Spreads"][i], "Spread", &rate, true)){
                            tempError = true;
                        }

                        if(this->qGetRawDate(JSON["Spreads"][i], "Date", &date, true)){
                            tempError = true;
                        }
                        if(!tempError){
                            this->spreadDates.push_back(date);
                            this->spreadRates.push_back(Handle<Quote>(boost::shared_ptr<Quote> (new SimpleQuote(rate))));
                        }

                        i++;
                    }
                }

            } else {
                erroros << "Spreads: Not found ";
                CROW_LOG_DEBUG << "Spreads: Not found ";
                this->getErrorLog()->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }
        }
    }else{
        error = true;
    }

    return error;

}