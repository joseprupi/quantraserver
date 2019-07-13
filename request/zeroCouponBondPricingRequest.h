//
// Created by Josep Rubio on 6/26/17.
//

#ifndef QUANTRASERVER_ZEROCOUPONBONDPRICINGREQUEST_H
#define QUANTRASERVER_ZEROCOUPONBONDPRICINGREQUEST_H

#include "crow.h"
#include "../common/enums.h"
#include "../parser/QuantraParser.h"
#include "../parser/zeroCouponBondParser.h"
#include "../parser/indexParser.h"
#include "../parser/scheduleParser.h"
#include "../parser/termStructureListParser.h"
#include "../parser/yieldParser.h"
#include "../parser/pricingParser.h"
#include "rapidjson.h"

using namespace rapidjson;

class zeroCouponBondPricingRequest {

public:
    void processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
};

#endif //QUANTRASERVER_ZEROCOUPONBONDPRICINGREQUEST_H
