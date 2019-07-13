//
// Created by Josep Rubio on 6/14/17.
//

#ifndef QUANTRASERVER_FLOATINGRATEBONDREQUEST_H
#define QUANTRASERVER_FLOATINGRATEBONDREQUEST_H

#include "crow.h"
#include "../common/enums.h"
#include "../parser/QuantraParser.h"
#include "../parser/floatingRateBondParser.h"
#include "../parser/indexParser.h"
#include "../parser/scheduleParser.h"
#include "../parser/termStructureListParser.h"
#include "../parser/yieldParser.h"
#include "../parser/pricingParser.h"
#include "rapidjson.h"

using namespace rapidjson;

class floatingRateBondPricingRequest {

public:
    void processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
};


#endif //QUANTRASERVER_FLOATINGRATEBONDREQUEST_H
