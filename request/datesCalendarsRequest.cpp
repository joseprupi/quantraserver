//
// Created by Josep Rubio on 7/19/17.
//

#include "datesCalendarsRequest.h"

void datesCalendarsRequest::isLeapYear(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    bool isLeap = false;
    QuantraParser parser("Is Leap", errorLog);

    try {

        int year;
        tmpBool = parser.qGetInt(d, "Year", &year, true);

        if (!tmpBool) {
            isLeap = Date::isLeap(year);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "IsLeap: "  << re.what();
        CROW_LOG_DEBUG << "IsLeap: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        (*response)["response"] = "ok";
        (*response)["message"]["IsLeap"]= isLeap;

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::endOfMonth(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    Date endOfMonth;
    QuantraParser parser("Is Leap", errorLog);

    try {

        Date date;
        tmpBool = parser.qGetRawDate(d, "Date", &date, true);

        if (!tmpBool) {
            endOfMonth = Date::endOfMonth(date);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "IsLeap: "  << re.what();
        CROW_LOG_DEBUG << "IsLeap: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        std::ostringstream os;
        os << QuantLib::io::iso_date(endOfMonth);
        (*response)["response"] = "ok";
        (*response)["message"]["EndOfMonth"]= os.str();

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::isEndOfMonth(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    bool isEndOfMonth = false;
    QuantraParser parser("Is Leap", errorLog);

    try {

        Date date;
        tmpBool = parser.qGetRawDate(d, "Date", &date, true);

        if (!tmpBool) {
            isEndOfMonth = Date::isEndOfMonth(date);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "IsLeap: "  << re.what();
        CROW_LOG_DEBUG << "IsLeap: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        (*response)["response"] = "ok";
        (*response)["message"]["IsEndOfMonth"]= isEndOfMonth;

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::nextWeekDay(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    Date nextWeekDay;
    Weekday weekday;

    QuantraParser parser("Next Week Day", errorLog);

    try {

        Date date;
        tmpBool = parser.qGetRawDate(d, "Date", &date, true);
        tmpBool = tmpBool || parser.qGetWeekDay(d, "Weekday", &weekday, true);

        if (!tmpBool) {
            nextWeekDay = Date::nextWeekday(date, weekday);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "NextWeekDay: "  << re.what();
        CROW_LOG_DEBUG << "NextWeekDay: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        std::ostringstream os;
        os << QuantLib::io::iso_date(nextWeekDay);
        (*response)["response"] = "ok";
        (*response)["message"]["NextWeekDay"]= os.str();

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::nthWeekDay(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    int nth;
    Weekday weekday;
    Month m;
    Year y;

    Date date;

    QuantraParser parser("Nth Week Day", errorLog);

    try {

        tmpBool = parser.qGetInt(d, "Nth", &nth, true);
        tmpBool = tmpBool || parser.qGetWeekDay(d, "Weekday", &weekday, true);
        tmpBool = tmpBool || parser.qGetMonth(d, "Month", &m, true);
        tmpBool = tmpBool ||parser.qGetInt(d, "Year", &y, true);

        if (!tmpBool) {
            date = Date::nthWeekday(nth, weekday, m, y);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "nthWeekDay: "  << re.what();
        CROW_LOG_DEBUG << "nthWeekDay: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        std::ostringstream os;
        os << QuantLib::io::iso_date(date);
        (*response)["response"] = "ok";
        (*response)["message"]["NthWeekDay"]= os.str();

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::isBusinessDay(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    bool isBusinessDate = false;
    QuantraParser parser("Is Business Day", errorLog);

    try {

        Date date;
        Calendar calendar;
        tmpBool = parser.qGetRawDate(d, "Date", &date, true);
        tmpBool = tmpBool || parser.qGetCalendar(d, "Calendar", &calendar, true);

        if (!tmpBool) {
            isBusinessDate = calendar.isBusinessDay(date);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "IsBusinessDay: "  << re.what();
        CROW_LOG_DEBUG << "IsBusinessDay: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        (*response)["response"] = "ok";
        (*response)["message"]["IsBusinessDay"]= isBusinessDate;

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::isHoliday(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    bool isBusinessDate = false;
    QuantraParser parser("Is Holiday", errorLog);

    try {

        Date date;
        Calendar calendar;
        tmpBool = parser.qGetRawDate(d, "Date", &date, true);
        tmpBool = tmpBool || parser.qGetCalendar(d, "Calendar", &calendar, true);

        if (!tmpBool) {
            isBusinessDate = calendar.isHoliday(date);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "IsHoliday: "  << re.what();
        CROW_LOG_DEBUG << "IsHoliday: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        (*response)["response"] = "ok";
        (*response)["message"]["IsHoliday"]= isBusinessDate;

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::holidayList(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    std::vector<Date> holidayList;
    QuantraParser parser("HolidayList", errorLog);

    try {

        Date from;
        Date to;
        bool includeWeekends;
        Calendar calendar;
        tmpBool = parser.qGetRawDate(d, "From", &from, true);
        tmpBool = parser.qGetRawDate(d, "To", &to, true);
        tmpBool = parser.qBool(d, "IncludeWeekends", &includeWeekends, true);
        tmpBool = tmpBool || parser.qGetCalendar(d, "Calendar", &calendar, true);

        if (!tmpBool) {
            holidayList = Calendar::holidayList(calendar, from, to, includeWeekends);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "HolidayList: "  << re.what();
        CROW_LOG_DEBUG << "HolidayList: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        (*response)["response"] = "ok";

        std::ostringstream os;

        for (int i = 0; i < holidayList.size(); i++) {
            os << QuantLib::io::iso_date(holidayList[i]);
            (*response)["message"]["HolidayList"][i] = os.str();
            os.str("");
            os.clear();
        }

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::adjust(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    Date adjustedDate;
    QuantraParser parser("AdjustDate", errorLog);

    try {

        Date date;
        Calendar calendar;
        BusinessDayConvention convention;
        tmpBool = parser.qGetRawDate(d, "Date", &date, true);
        tmpBool = tmpBool || parser.qGetCalendar(d, "Calendar", &calendar, true);
        tmpBool = tmpBool || parser.qGetConvention(d, "Convention", &convention, true);

        if (!tmpBool) {
            adjustedDate = calendar.adjust(date, convention);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "AdjustDate: "  << re.what();
        CROW_LOG_DEBUG << "AdjustDate: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        std::ostringstream os;
        os << QuantLib::io::iso_date(adjustedDate);

        (*response)["response"] = "ok";
        (*response)["message"]["AdjustedDate"]= os.str();

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::advance(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    Date adjustedDate;
    QuantraParser parser("AdvancedDate", errorLog);

    try {

        Date date;
        Calendar calendar;
        BusinessDayConvention convention;
        Integer n;
        TimeUnit unit;
        bool endOfMonth;

        tmpBool = parser.qGetRawDate(d, "Date", &date, true);
        tmpBool = tmpBool || parser.qGetCalendar(d, "Calendar", &calendar, true);
        tmpBool = tmpBool || parser.qGetConvention(d, "Convention", &convention, true);
        tmpBool = tmpBool || parser.qGetInt(d, "Number", &n, true);
        tmpBool = tmpBool || parser.qTimeUnit(d, "TimeUnit", &unit, true);
        tmpBool = tmpBool || parser.qBool(d, "EndOfMonth", &endOfMonth, true);

        if (!tmpBool) {
            adjustedDate = calendar.advance(date,n,unit,convention,endOfMonth);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "AdvancedDate: "  << re.what();
        CROW_LOG_DEBUG << "AdvancedDate: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        std::ostringstream os;
        os << QuantLib::io::iso_date(adjustedDate);

        (*response)["response"] = "ok";
        (*response)["message"]["AdvancedDate"]= os.str();

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::businessDaysBetween(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    int daysBetween;
    QuantraParser parser("BusinessDaysBetween", errorLog);

    try {

        Date from;
        Date to;
        bool includeFirst;
        bool includeLast;
        Calendar calendar;
        tmpBool = parser.qGetRawDate(d, "From", &from, true);
        tmpBool = tmpBool || parser.qGetRawDate(d, "To", &to, true);
        tmpBool = tmpBool || parser.qBool(d, "IncludeFirst", &includeFirst, true);
        tmpBool = tmpBool || parser.qBool(d, "IncludeLast", &includeLast, true);
        tmpBool = tmpBool || parser.qGetCalendar(d, "Calendar", &calendar, true);

        if (!tmpBool) {
            daysBetween = calendar.businessDaysBetween(from,to,includeFirst,includeLast);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "BusinessDaysBetween: "  << re.what();
        CROW_LOG_DEBUG << "BusinessDaysBetween: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        (*response)["response"] = "ok";

        (*response)["response"] = "ok";
        (*response)["message"]["BusinessDaysBetween"]= daysBetween;

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::isIMMdate(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    bool isIMMdate = false;
    QuantraParser parser("IsIMMdate", errorLog);

    try {

        Date date;
        tmpBool = parser.qGetRawDate(d, "Date", &date, true);

        if (!tmpBool) {
            isIMMdate = IMM::isIMMdate(date,true);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "IsIMMdate: "  << re.what();
        CROW_LOG_DEBUG << "IsIMMdate: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        (*response)["response"] = "ok";
        (*response)["message"]["IsIMMdate"]= isIMMdate;

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::nextIMMdate(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    Date IMMdate;
    QuantraParser parser("nextIMMdate", errorLog);

    try {

        Date date;
        tmpBool = parser.qGetRawDate(d, "Date", &date, true);

        if (!tmpBool) {
            IMMdate = IMM::nextDate(date,true);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "nextIMMdate: "  << re.what();
        CROW_LOG_DEBUG << "nextIMMdate: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        std::ostringstream os;
        os << QuantLib::io::iso_date(IMMdate);

        (*response)["response"] = "ok";
        (*response)["message"]["NextIMMdate"]= os.str();

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}

void datesCalendarsRequest::yearFraction(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["Errors"] = erroros.str();

        return;
    }

    Time fraction;
    QuantraParser parser("yearFraction", errorLog);

    try {

        Date startDate;
        Date endDate;
        DayCounter dayCounter;

        tmpBool = parser.qGetRawDate(d, "StartDate", &startDate, true);
        tmpBool = tmpBool || parser.qGetRawDate(d, "EndDate", &endDate, true);
        tmpBool = tmpBool || parser.qGetDayCounter(d, "DayCounter", &dayCounter, true);

        if (!tmpBool) {
            fraction = dayCounter.yearFraction(startDate, endDate);
        }

    }catch (Error re) {
        tmpBool = true;
        erroros << "yearFraction: "  << re.what();
        CROW_LOG_DEBUG << "yearFraction: " << re.what();
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
    }

    if(!tmpBool){

        (*response)["response"] = "ok";
        (*response)["message"]["YearFraction"]= fraction;

    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["Errors"][i] = *it;
            i++;

        }
    }
}