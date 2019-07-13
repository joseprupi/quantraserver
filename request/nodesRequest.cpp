//
// Created by Josep Rubio on 6/22/17.
//

#include "nodesRequest.h"

void nodesRequest::processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["NodesErrors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["NodesErrors"] = erroros.str();

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

    std::vector<curvePoint> resultPoints;

    if(!error){

        termStructureListParser TermStructureListParser("Curves",errorLog);
        iterator = d.FindMember("Curves");
        if(iterator != d.MemberEnd()){
            TermStructureListParser.setAsOfDate(PricingParser.getAsOfDate());
            tmpBool = TermStructureListParser.parse(d);
            error = error || tmpBool;
        }else{
            erroros << "Curves: Not found ";
            CROW_LOG_DEBUG << "Curves: Not found ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }

        if(!error){

            try {

                boost::shared_ptr<termStructureParser> term;

                std::map<string,boost::shared_ptr<termStructureParser>> list =
                        TermStructureListParser.getTermStructuresList();

                std::map<string,boost::shared_ptr<termStructureParser>>::iterator it;
                it =list.find("NodesTermStructure");

                if (it != list.end()) {
                    term = it->second;

                }else {
                    erroros << "NodesTermStructure not found ";
                    CROW_LOG_DEBUG << "NodesTermStructure not found ";
                    errorLog->push_back(erroros.str());
                    erroros.str("");
                    erroros.clear();
                    error = true;
                }

                if (!error) {

                    int i = 0;
                    for (std::vector<std::pair<Date, Real>>::iterator it = term->nodes.begin();
                         it != term->nodes.end();
                         ++it) {

                        curvePoint resultPoint;
                        resultPoint.point = i;

                        std::ostringstream os;
                        os << QuantLib::io::iso_date(it->first);
                        resultPoint.date = os.str();
                        resultPoint.rate = it->second;

                        resultPoints.push_back(resultPoint);

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
    }

    if(!error){

        (*response)["response"] = "ok";

        for (int i = 0; i < resultPoints.size(); i++) {

            (*response)["message"][i]["Point"] = resultPoints[i].point;
            (*response)["message"][i]["Date"] = resultPoints[i].date;
            (*response)["message"][i]["Rate"] = resultPoints[i].rate;

        }
    }else{
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["NodesErrors"][i] = *it;
            i++;

        }
    }
}