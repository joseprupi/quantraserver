//
// Created by Josep Rubio on 6/26/17.
//

#include "volatilitySurfaceRequest.h"

void volatilitySurfaceRequest::processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["VolatilitySurfaceErrors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["VolatilitySurfaceErrors"] = erroros.str();

        return;
    }

    Value::MemberIterator iterator;
    pricingParser PricingParser(errorLog, "Pricing");
    iterator = d.FindMember("Pricing");
    if (iterator != d.MemberEnd()) {
        Value &pricingJSON = d["Pricing"];
        tmpBool = PricingParser.parse(pricingJSON);
        error = error || tmpBool;
    } else {
        erroros << "Pricing: Not found ";
        CROW_LOG_DEBUG << "Pricing: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    volatilitySurfaceParser VolatilitySurfaceParser(errorLog, "VolatilitySurface");
    iterator = d.FindMember("VolatilitySurface");
    if (iterator != d.MemberEnd()) {
        Value &volatilitySurfaceJSON = d["VolatilitySurface"];
        tmpBool = VolatilitySurfaceParser.parse(volatilitySurfaceJSON);
        error = error || tmpBool;
    } else {
        erroros << "VolatilitySurface: Not found ";
        CROW_LOG_DEBUG << "VolatilitySurface: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    volTermStructureParser VolTermStructureParser(errorLog, "VolTermStructure");
    iterator = d.FindMember("VolTermStructure");
    if (iterator != d.MemberEnd()) {
        Value &volTermStructureJSON = d["VolTermStructure"];
        tmpBool = VolTermStructureParser.parse(volTermStructureJSON);
        error = error || tmpBool;
    } else {
        erroros << "VolTermStructureParser: Not found ";
        CROW_LOG_DEBUG << "VolTermStructureParser: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    if(!error){

        try{

            VolTermStructureParser.setDate(PricingParser.getAsOfDate());

            Settings::instance().evaluationDate() = PricingParser.getAsOfDate();

            boost::shared_ptr<BlackVarianceSurface> term =
                    boost::dynamic_pointer_cast<BlackVarianceSurface>(VolTermStructureParser.createVolTermStructure());

            Date initExpiration = VolatilitySurfaceParser.getInitDate();
            Date endExpiration = VolatilitySurfaceParser.getEndDate();

            double initStrike = VolatilitySurfaceParser.getInitStrike();
            double endStrike = VolatilitySurfaceParser.getEndStrike();

            int expirationStep = VolatilitySurfaceParser.getExpirationStep();
            double strikeStep = VolatilitySurfaceParser.getStrikeStep();

            int i = 0;
            for(Date date = initExpiration; date < endExpiration; date = date + expirationStep){
                for(double strike = initStrike; strike < endStrike; strike = strike + strikeStep){

                    std::ostringstream os;
                    os << QuantLib::io::iso_date(date);

                    (*response)["message"][i]["Expiration"] = os.str();
                    (*response)["message"][i]["Strike"] =strike;
                    (*response)["message"][i]["Volatility"] = term->blackVol(date, strike, true);
                    i++;
                }
            }


        }catch (Error e) {
                    erroros << e.what();
                    CROW_LOG_DEBUG << e.what();
                    errorLog->push_back(erroros.str());
                    erroros.str("");
                    erroros.clear();
                    error = true;
        }

    }

    if(error){
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["VolatilitySurfaceErrors"][i] = *it;
            i++;

        }
    }
}