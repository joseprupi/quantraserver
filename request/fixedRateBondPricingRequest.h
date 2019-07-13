//
// Created by Josep Rubio on 6/30/17.
//

#ifndef QUANTRASERVER_FIXEDRATEBONDPRICINGREQUEST_H
#define QUANTRASERVER_FIXEDRATEBONDPRICINGREQUEST_H

#include "crow.h"
#include "../common/enums.h"
#include "../parser/QuantraParser.h"
#include "../parser/fixedRateBondParser.h"
#include "../parser/scheduleParser.h"
#include "../parser/termStructureListParser.h"
#include "../parser/yieldParser.h"
#include "../parser/pricingParser.h"
#include "rapidjson.h"

class fixedRateBondPricingRequest {
public:
    void processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
};


#endif //QUANTRASERVER_FIXEDRATEBONDPRICINGREQUEST_H
