//
// Created by Josep Rubio on 6/26/17.
//

#ifndef QUANTRASERVER_VOILATILITYSURFACEREQUEST_H
#define QUANTRASERVER_VOILATILITYSURFACEREQUEST_H


#include "crow.h"
#include "../common/common.h"
#include "../common/enums.h"
#include "../parser/QuantraParser.h"
#include "../parser/volatilitySurfaceParser.h"
#include "../parser/volTermStructureParser.h"
#include "../parser/pricingParser.h"
#include "rapidjson.h"

using namespace rapidjson;

class volatilitySurfaceRequest {

public:
    void processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response);
};


#endif //QUANTRASERVER_VOILATILITYSURFACEREQUEST_H
