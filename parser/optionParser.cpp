//
// Created by Josep Rubio on 6/19/17.
//

#include "optionParser.h"

optionParser::optionParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;
    this->isMultiOption = false;

}

OptionStyleEnum optionParser::getOptionStyle(){
    return this->optionStyle;
}

boost::shared_ptr<europeanOptionParser> optionParser::getEuropeanOptionParser(){
    return this->EuropeanOptionParser;
}

boost::shared_ptr<europeanMultiOptionParser> optionParser::getEuropeanMultiOptionParser(){
    return this->EuropeanMultiOptionParser;
}

void optionParser::setMultioption(bool isMultiOption){
    this->isMultiOption = isMultiOption;
}

bool optionParser::parse(Value &JSON) {

    bool error = false;
    ostringstream erroros;

    if(this->qGetOptionStyle(JSON, "Style", &optionStyle, true)){
        error = true;
    }

    if(this->isMultiOption){
        Value::MemberIterator iterator;
        if (optionStyle == EuropeanString) {
            iterator = JSON.FindMember("EuropeanOption");
            if (iterator != JSON.MemberEnd()) {
                Value &europeanOptionJSON = JSON["EuropeanOption"];
                this->EuropeanMultiOptionParser = boost::shared_ptr<europeanMultiOptionParser>(
                        new europeanMultiOptionParser(this->getErrorLog(), "EuropeanOption"));
                error = error || this->EuropeanMultiOptionParser->parse(europeanOptionJSON);
            } else {
                erroros << "EuropeanOption: Not found ";
                CROW_LOG_DEBUG << "EuropeanOption: Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }
        } else {
            erroros << "Option Style not supported";
            CROW_LOG_DEBUG << "Option Style not supported";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }
    }else {
        Value::MemberIterator iterator;
        if (optionStyle == EuropeanString) {
            iterator = JSON.FindMember("EuropeanOption");
            if (iterator != JSON.MemberEnd()) {
                Value &europeanOptionJSON = JSON["EuropeanOption"];
                this->EuropeanOptionParser = boost::shared_ptr<europeanOptionParser>(
                        new europeanOptionParser(this->getErrorLog(), "EuropeanOption"));
                error = error || this->EuropeanOptionParser->parse(europeanOptionJSON);
            } else {
                erroros << "EuropeanOption: Not found ";
                CROW_LOG_DEBUG << "EuropeanOption: Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }
        } else {
            erroros << "Option Style not supported";
            CROW_LOG_DEBUG << "Option Style not supported";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }
    }

    return error;

}