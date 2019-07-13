//
// Created by Josep Rubio on 6/26/17.
//

#ifndef QUANTRASERVER_OPTIONMULTIPRICINGREQUEST_H
#define QUANTRASERVER_OPTIONMULTIPRICINGREQUEST_H


#include "crow.h"
#include "../common/common.h"
#include "../common/enums.h"
#include "../parser/QuantraParser.h"
#include "../parser/optionParser.h"
#include "../parser/volTermStructureParser.h"
#include "../parser/pricingParser.h"
#include "../parser/termStructureListParser.h"
#include "rapidjson.h"

using namespace rapidjson;

class optionMultiPricingRrequest {

public:
    void processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
};


#endif //QUANTRASERVER_OPTIONMULTIPRICINGREQUEST_H
