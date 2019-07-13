//
// Created by Josep Rubio on 7/19/17.
//

#ifndef QUANTRASERVER_DATESCALENDARSREQUEST_H
#define QUANTRASERVER_DATESCALENDARSREQUEST_H

#include "crow.h"
#include "../common/common.h"
#include "../common/enums.h"
#include "../parser/QuantraParser.h"
#include "../parser/floatingRateBondParser.h"
#include "../parser/indexParser.h"
#include "../parser/scheduleParser.h"
#include "../parser/termStructureParser.h"
#include "../parser/yieldParser.h"
#include "../parser/pricingParser.h"
#include "rapidjson.h"

using namespace rapidjson;

class datesCalendarsRequest {

public:

    void isLeapYear(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void endOfMonth(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void isEndOfMonth(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void nextWeekDay(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void nthWeekDay(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void isBusinessDay(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void isHoliday(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void holidayList(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void adjust(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void advance(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void businessDaysBetween(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void isIMMdate(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void nextIMMdate(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
    void yearFraction(crow::request req, boost::shared_ptr<crow::json::wvalue> response);

};


#endif //QUANTRASERVER_DATESCALENDARSREQUEST_H
