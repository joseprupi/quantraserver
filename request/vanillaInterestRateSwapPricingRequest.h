//
// Created by Josep Rubio on 7/6/17.
//

#ifndef QUANTRASERVER_VANILLAINTERESTRATESWAPPRICING_H
#define QUANTRASERVER_VANILLAINTERESTRATESWAPPRICING_H


#include "crow.h"
#include "../common/enums.h"
#include "../parser/QuantraParser.h"
#include "../parser/zeroCouponBondParser.h"
#include "../parser/indexParser.h"
#include "../parser/scheduleParser.h"
#include "../parser/termStructureListParser.h"
#include "../parser/yieldParser.h"
#include "../parser/pricingParser.h"
#include "../parser/vanillaInterestRateSwapParser.h"
#include "rapidjson.h"

using namespace rapidjson;

class vanillaInterestRateSwapPricingRequest {

public:
    void processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
};

#endif //QUANTRASERVER_VANILLAINTERESTRATESWAPPRICING_H
