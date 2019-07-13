//
// Created by Josep Rubio on 6/23/17.
//

#include "fwdRateCurveRequest.h"

void fwdRateCurveRequest::processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["FwdRateCurveErrors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["FwdRateCurveErrors"] = erroros.str();

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

    fwdCurveParser FwdCurveParser(errorLog, "FwdCurve");
    iterator = d.FindMember("FwdCurve");
    if(iterator != d.MemberEnd()){
        Value& fwdCurveJSON = d["FwdCurve"];
        tmpBool = FwdCurveParser.parse(fwdCurveJSON);
        error = error || tmpBool;
    }else{
        erroros << "FwdCurve: Not found ";
        CROW_LOG_DEBUG << "FwdCurve: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    std::vector<curvePoint> resultPoints;

    if(!error) {

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

        if (!error) {

            try {

                boost::shared_ptr<YieldTermStructure> term;

                std::map<string,boost::shared_ptr<termStructureParser>> list =
                        TermStructureListParser.getTermStructuresList();

                std::map<string,boost::shared_ptr<termStructureParser>>::iterator it;
                it =list.find("FwdTermStructure");

                if (it != list.end()) {
                    term = it->second->getTermStructure();

                }else {
                    erroros << "FwdTermStructure not found ";
                    CROW_LOG_DEBUG << "FwdTermStructure not found ";
                    errorLog->push_back(erroros.str());
                    erroros.str("");
                    erroros.clear();
                    error = true;
                }

                if (!error) {

                    Date counter = PricingParser.getAsOfDate();

                    int i = 0;

                    if (FwdCurveParser.getDates()) {
                        curvePoint resultPoint;
                        resultPoint.point = i;

                        std::ostringstream os;
                        os << QuantLib::io::iso_date(counter);
                        resultPoint.date = os.str();

                        resultPoint.rate = term->forwardRate(FwdCurveParser.getStartDate(),
                                                                   FwdCurveParser.getEndDate(),
                                                                   FwdCurveParser.getDayCounter(),
                                                                   FwdCurveParser.getCompounding()).rate();

                        resultPoints.push_back(std::move(resultPoint));
                    } else {
                        while (counter < term->maxDate() - FwdCurveParser.getFwdCurvePeriodTimeUnit() *
                                                           FwdCurveParser.getFwdCurvePeriodNumber()) {

                            curvePoint resultPoint;
                            resultPoint.point = i;

                            std::ostringstream os;
                            os << QuantLib::io::iso_date(counter);
                            resultPoint.date = os.str();

                            resultPoint.rate = term.get()->forwardRate(counter, FwdCurveParser.getFwdCalendar().advance(
                                                                               counter,
                                                                               FwdCurveParser.getFwdCurvePeriodNumber(),
                                                                               FwdCurveParser.getFwdCurvePeriodTimeUnit()),
                                                                       FwdCurveParser.getDayCounter(),
                                                                       FwdCurveParser.getCompounding(), Annual,
                                                                       true).rate();

                            resultPoints.push_back(std::move(resultPoint));

                            counter++;
                            i++;
                        }
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

            (*response)["message"]["FwdRateCurveErrors"][i] = *it;
            i++;

        }
    }
}
