//
// Created by Josep Rubio on 6/22/17.
//

#ifndef QUANTRASERVER_ZERORATECURVEREQUEST_H
#define QUANTRASERVER_ZERORATECURVEREQUEST_H


#include "crow.h"
#include "../common/common.h"
#include "../common/enums.h"
#include "../parser/QuantraParser.h"
#include "../parser/floatingRateBondParser.h"
#include "../parser/indexParser.h"
#include "../parser/scheduleParser.h"
#include "../parser/termStructureListParser.h"
#include "../parser/yieldParser.h"
#include "../parser/pricingParser.h"
#include "../parser/zeroCurveParser.h"
#include "rapidjson.h"

using namespace rapidjson;

class zeroRateCurveRequest {

public:
    void processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
};


#endif //QUANTRASERVER_ZERORATECURVEREQUEST_H
