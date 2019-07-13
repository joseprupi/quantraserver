//
// Created by Josep Rubio on 6/19/17.
//

#include "volatilitySurfaceParser.h"

volatilitySurfaceParser::volatilitySurfaceParser(std::vector<std::string> *errorLog, std::string message)
        :QuantraParser(message, errorLog){

    this->message = message;

}

int volatilitySurfaceParser::getExpirationStep(){
    return this->expirationStep;
}

double volatilitySurfaceParser::getStrikeStep(){
    return this->strikeStep;
}

double volatilitySurfaceParser::getInitStrike(){
    return this->initStrike;
}

double volatilitySurfaceParser::getEndStrike(){
    return this->endStrike;
}

Date volatilitySurfaceParser::getInitDate(){
    return this->initDate;
}

Date volatilitySurfaceParser::getEndDate(){
    return this->endDate;
}

bool volatilitySurfaceParser::parse(Value &JSON) {

    bool error = false;

    if(this->qGetInt(JSON, "ExpirationStep", &expirationStep, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "StrikeStep", &strikeStep, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "InitStrike", &initStrike, true)){
        error = true;
    }

    if(this->qGetDouble(JSON, "EndStrike", &endStrike, true)){
        error = true;
    }

    if(this->qGetRawDate(JSON, "InitDate", &initDate, true)){
        error = true;
    }

    if(this->qGetRawDate(JSON, "EndDate", &endDate, true)){
        error = true;
    }

    return error;

}