//
// Created by Josep Rubio on 6/16/17.
//

#ifndef QUANTRASERVER_QUANTRAPARSER_H
#define QUANTRASERVER_QUANTRAPARSER_H

#include <document.h>
#include <string>
#include <sstream>
#include <vector>
#include "rapidjson.h"
#include "../common/enums.h"
#include "error/en.h"

using namespace rapidjson;
using namespace std;

class QuantraParser {

private:

    string message;
    std::vector<std::string> *errorLog;
    ostringstream erroros;

public:
    QuantraParser(string message, std::vector<std::string> *errorLog);
    void setMessage(string message);
    //void setErrorLog(boost::shared_ptr<std::vector<string>> errorLog);
    std::vector<string>* getErrorLog();
    string getMessage();

    bool qGetDouble(Value &JSON, const char* field, double *value, bool mandatory);
    bool qGetDoubleArray(Value &JSON, const char* field, double *value, bool mandatory);
    bool qGetInt(Value &JSON, const char* field, int *value, bool mandatory);
    bool qGetString(Value &JSON, const char* field, string *value, bool mandatory);
    bool qGetDayCounter(Value &JSON, const char* field, DayCounter *value, bool mandatory);
    bool qGetConvention(Value &JSON, const char* field, BusinessDayConvention *value, bool mandatory);
    bool qGetDate(Value &JSON, const char* field, Date *value, bool mandatory, Calendar cal);
    bool qGetRawDate(Value &JSON, const char* field, Date *value, bool mandatory);
    bool qGetDateArray(Value &JSON, const char* field, Date *value, bool mandatory);
    bool qGetCalendar(Value &JSON, const char* field, Calendar *value, bool mandatory);
    bool qGetPeriod(Value &JSON, const char* field, Frequency *value, bool mandatory);
    bool qDateGenerationRule(Value &JSON, const char* field, DateGeneration::Rule *value, bool mandatory);
    bool qBool(Value &JSON, const char* field, bool *value, bool mandatory);
    bool qTimeUnit(Value &JSON, const char* field, TimeUnit *value, bool mandatory);
    bool qInterpolator(Value &JSON, const char* field, InterpolationEnum *value, bool mandatory);
    bool qCompounding(Value &JSON, const char* field, Compounding *value, bool mandatory);
    bool qIborIndex(Value &JSON, const char* field, boost::shared_ptr<IborIndex>&, bool mandatory);
    bool qGetOvernightIndex(Value &JSON, const char* field, boost::shared_ptr<OvernightIndex>& value, bool mandatory);
    bool qPointType(Value &JSON, const char* field, PointType *value, bool mandatory);
    bool qGetSwapType(Value &JSON, const char* field, VanillaSwap::Type *value, bool mandatory);
    bool qGetWeekDay(Value &JSON, const char* field, Weekday *value, bool mandatory);
    bool qGetMonth(Value &JSON, const char* field, Month *value, bool mandatory);
    bool qGetBootstrapTrait(Value &JSON, const char* field, BootstrapTrait *value, bool mandatory);
    bool qGetZeroSpreadType(Value &JSON, const char* field, ZeroSpreadTypeEnum *value, bool mandatory);
    bool qGetVolType(Value &JSON, const char* field, VolEnum *value, bool mandatory);
    bool qGetOptionType(Value &JSON, const char* field, Option::Type *value, bool mandatory);
    bool qGetOptionStyle(Value &JSON, const char* field, OptionStyleEnum *value, bool mandatory);
};


#endif //QUANTRASERVER_QUANTRAPARSER_H
