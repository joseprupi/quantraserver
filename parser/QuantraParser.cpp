//
// Created by Josep Rubio on 6/16/17.
//

#include <crow/logging.h>
#include <boost/smart_ptr/shared_ptr.hpp>
#include "QuantraParser.h"

QuantraParser::QuantraParser(string message, std::vector<std::string> *errorLog) {
    this->message = message;
    this->errorLog = errorLog;
}

void QuantraParser::setMessage(string message){
    this->message = message;
}

std::vector<string>* QuantraParser::getErrorLog(){
    return this->errorLog;
}

string QuantraParser::getMessage(){
    return this->message;
}

bool QuantraParser::qGetDouble(Value &JSON, const char* field, double *value, bool mandatory){

    bool error = false;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember(field);
    if(iterator != JSON.MemberEnd()){
        if(iterator->value.IsNumber()){
            (*value) = iterator->value.GetDouble();
        }else{
            error = true;
            erroros  << message << " " << field << ": Not a double number";
            CROW_LOG_DEBUG  << message << " " << field << ": Not an double number";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }
    }else{
        if(mandatory) {
            erroros << message << " " << field << ": Not found ";
            CROW_LOG_DEBUG << message << " " << field << ": Not found ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }else{
            error = false;
        }
    }

    return error;
}

bool QuantraParser::qGetDoubleArray(Value &JSON, const char* field, double *value, bool mandatory){

    bool error = false;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember(field);
    if(iterator != JSON.MemberEnd()){
        if (!JSON[field].IsArray() || JSON[field].GetArray().Size() == 0) {
            erroros << message << " " << field << ": has to be an array with at least 1 element ";
            CROW_LOG_DEBUG << message << " " << field << ": has to be an array with at least 1 element ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;

        } else {
            for (SizeType i = 0; i < JSON[field].Size(); i++){

                if(JSON[field][i].IsNumber()){
                    value[i] = JSON[field][i].GetDouble();
                }else{
                    error = true;
                    erroros  << message << " " << field << ": Not a double number";
                    CROW_LOG_DEBUG  << message << " " << field << ": Not an double number";
                    errorLog->push_back(erroros.str());
                    erroros.str("");
                    erroros.clear();
                }
            }
        }
    }else{
        if(mandatory) {
            erroros << message << " " << field << ": Not found ";
            CROW_LOG_DEBUG << message << " " << field << ": Not found ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }else{
            error = false;
        }
    }

    return error;
}

bool QuantraParser::qGetDateArray(Value &JSON, const char* field, Date *value, bool mandatory){

    bool error = false;

        try{

            Value::MemberIterator iterator;
            iterator = JSON.FindMember(field);
            if(iterator != JSON.MemberEnd()){
                if (!JSON[field].IsArray() || JSON[field].GetArray().Size() == 0) {
                    erroros << message << " " << field << ": has to be an array with at least 1 element ";
                    CROW_LOG_DEBUG << message << " " << field << ": has to be an array with at least 1 element ";
                    errorLog->push_back(erroros.str());
                    erroros.str("");
                    erroros.clear();
                    error = true;
            } else {
                for (SizeType i = 0; i < JSON[field].Size(); i++){

                    if (JSON[field][i].IsString()) {
                        Date issueDate = DateParser::parseFormatted(JSON[field][i].GetString(), "%Y/%m/%d");
                        value[i] = issueDate;
                    }else{
                        error = true;
                        erroros  << message << " " << field << ": Not a string";
                        CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                        errorLog->push_back(erroros.str());
                        erroros.str("");
                        erroros.clear();
                    }
                }
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }

    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}



bool QuantraParser::qGetInt(Value &JSON, const char* field, int *value, bool mandatory){

    bool error = false;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember(field);
    if(iterator != JSON.MemberEnd()){
        if(iterator->value.IsInt()){
            (*value) = iterator->value.GetInt();
        }else{
            error = true;
            erroros  << message << " " << field << ": Not a int number";
            CROW_LOG_DEBUG  << message << " " << field << ": Not an int number";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }
    }else{
        if(mandatory) {
            erroros << message << " " << field << ": Not found ";
            CROW_LOG_DEBUG << message << " " << field << ": Not found ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }else{
            error = false;
        }
    }

    return error;
}

bool QuantraParser::qGetString(Value &JSON, const char* field, string *value, bool mandatory){

    bool error = false;

    Value::MemberIterator iterator;
    iterator = JSON.FindMember(field);
    if(iterator != JSON.MemberEnd()){
        if(iterator->value.IsString()){
            (*value) = iterator->value.GetString();
        }else{
            error = true;
            erroros  << message << " " << field << ": Not a string";
            CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }
    }else{
        if(mandatory) {
            erroros << message << " " << field << ": Not found ";
            CROW_LOG_DEBUG << message << " " << field << ": Not found ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }else{
            error = false;
        }
    }

    return error;
}

bool QuantraParser::qGetDayCounter(Value &JSON, const char* field, DayCounter *value, bool mandatory){

    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = dayCounterToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetConvention(Value &JSON, const char* field, BusinessDayConvention *value, bool mandatory){

    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = conventionToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetDate(Value &JSON, const char* field, Date *value, bool mandatory, Calendar cal){
    bool error = false;

    try {
        if(cal.empty() == false) {
            Value::MemberIterator iterator;
            iterator = JSON.FindMember(field);
            if (iterator != JSON.MemberEnd()) {
                if (iterator->value.IsString()) {
                    if (validBusinessDate(iterator->value.GetString(), cal)) {
                        Date issueDate = DateParser::parseFormatted(iterator->value.GetString(), "%Y/%m/%d");
                        (*value) = issueDate;
                    } else {
                        error = true;
                        erroros << message << " " << field << " " << iterator->value.GetString() <<
                        " is not a valid business date";
                        CROW_LOG_DEBUG << " " << field << " " << iterator->value.GetString() <<
                                       " is not a valid business date";
                        errorLog->push_back(erroros.str());
                        erroros.str("");
                        erroros.clear();
                    }
                } else {
                    error = true;
                    erroros << message << " " << field << ": Not a string";
                    CROW_LOG_DEBUG << message << " " << field << ": Not a string";
                    errorLog->push_back(erroros.str());
                    erroros.str("");
                    erroros.clear();
                }
            } else {
                if (mandatory) {
                    erroros << message << " " << field << ": Not found ";
                    CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                    errorLog->push_back(erroros.str());
                    erroros.str("");
                    erroros.clear();
                    error = true;
                }else{
                    error = false;
                }
            }
        }else{
            error = true;
            erroros << message << " " << field << ": Not calendar for date found ";
            CROW_LOG_DEBUG << message << " " << field << ": Not calendar for date found ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetRawDate(Value &JSON, const char* field, Date *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if (iterator != JSON.MemberEnd()) {
            if (iterator->value.IsString()) {
                Date issueDate = DateParser::parseFormatted(iterator->value.GetString(), "%Y/%m/%d");
                (*value) = issueDate;
            } else {
                error = true;
                erroros << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        } else {
            if (mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetCalendar(Value &JSON, const char* field, Calendar *value, bool mandatory){

    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = calendarToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetPeriod(Value &JSON, const char* field, Frequency *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = frequencyToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qDateGenerationRule(Value &JSON, const char* field, DateGeneration::Rule *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = dateGenerationToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qBool(Value &JSON, const char* field, bool *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsBool()){
                (*value) = iterator->value.GetBool();
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a boolean";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a boolean";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qTimeUnit(Value &JSON, const char* field, TimeUnit *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = timeUnitToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qInterpolator(Value &JSON, const char* field, InterpolationEnum *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = interpolationToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qCompounding(Value &JSON, const char* field, Compounding *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = compoundingToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qIborIndex(Value &JSON, const char* field, boost::shared_ptr<IborIndex>& value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                value = iborToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetOvernightIndex(Value &JSON, const char* field, boost::shared_ptr<OvernightIndex>& value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                value = overnightIndexToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetSwapType(Value &JSON, const char* field, VanillaSwap::Type *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = swapTypeToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qPointType(Value &JSON, const char* field, PointType *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = pointTypeToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetWeekDay(Value &JSON, const char* field, Weekday *value, bool mandatory){

    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = weekDayToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetMonth(Value &JSON, const char* field, Month *value, bool mandatory){

    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = monthToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetBootstrapTrait(Value &JSON, const char* field, BootstrapTrait *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = bootstrapTraitToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetZeroSpreadType(Value &JSON, const char* field, ZeroSpreadTypeEnum *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = ZeroSpreadTypeToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetVolType(Value &JSON, const char* field, VolEnum *value, bool mandatory){
    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = volToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetOptionType(Value &JSON, const char* field, Option::Type *value, bool mandatory){

    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = optionTypeToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}

bool QuantraParser::qGetOptionStyle(Value &JSON, const char* field, OptionStyleEnum *value, bool mandatory){

    bool error = false;

    try {
        Value::MemberIterator iterator;
        iterator = JSON.FindMember(field);
        if(iterator != JSON.MemberEnd()){
            if(iterator->value.IsString()){
                (*value) = OptionStyleToQL(iterator->value.GetString());
            }else{
                error = true;
                erroros  << message << " " << field << ": Not a string";
                CROW_LOG_DEBUG  << message << " " << field << ": Not a string";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
            }
        }else{
            if(mandatory) {
                erroros << message << " " << field << ": Not found ";
                CROW_LOG_DEBUG << message << " " << field << ": Not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }else{
                error = false;
            }
        }
    }catch (Error re) {
        error = true;
        erroros  << message << " " << field << " " << re.what();
        CROW_LOG_DEBUG  << message << " " << field << " " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    return error;
}