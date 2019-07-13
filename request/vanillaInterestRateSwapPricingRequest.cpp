//
// Created by Josep Rubio on 7/6/17.
//

#include "vanillaInterestRateSwapPricingRequest.h"

void vanillaInterestRateSwapPricingRequest::processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response){

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["VanillaInterestRateSwapErrors"] = "Wrong format";
        return;
    }

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["VanillaInterestRateSwapErrors"] = erroros.str();

        return;
    }

    vanillaInterestRateSwapParser SwapParser(errorLog, "VanillaInterestRateSwap");
    Value::MemberIterator iterator;
    iterator = d.FindMember("VanillaInterestRateSwap");
    if(iterator != d.MemberEnd()){
        Value& JSON = d["VanillaInterestRateSwap"];
        tmpBool = SwapParser.parse(JSON);
        error = error || tmpBool;
    }else{
        erroros << "VanillaInterestRateSwap: Not found ";
        CROW_LOG_DEBUG << "VanillaInterestRateSwap: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    pricingParser PricingParser(errorLog, "Pricing");
    iterator = d.FindMember("Pricing");
    if(iterator != d.MemberEnd()){
        Value& pricingJSON = d["Pricing"];
        tmpBool = PricingParser.parse(pricingJSON);
        error = error || tmpBool;
    }else{
        erroros << "Pricing: Not found ";
        CROW_LOG_DEBUG << "Pricing: Not found ";
        errorLog->push_back(erroros.str());
        erroros.str("");
        erroros.clear();
        error = true;
    }

    boost::shared_ptr<YieldTermStructure> discountingTermStructure;
    boost::shared_ptr<YieldTermStructure> forecastingTermStructure;

    if(!error) {

        termStructureListParser TermStructureListParser("Curves", errorLog);
        iterator = d.FindMember("Curves");
        if (iterator != d.MemberEnd()) {
            TermStructureListParser.setAsOfDate(PricingParser.getAsOfDate());
            tmpBool = TermStructureListParser.parse(d);
            error = error || tmpBool;
        } else {
            erroros << "Curves: Not found ";
            CROW_LOG_DEBUG << "Curves: Not found ";
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
            error = true;
        }

        if (!error) {

            std::map<string,boost::shared_ptr<termStructureParser>> list =
                    TermStructureListParser.getTermStructuresList();

            std::map<string, boost::shared_ptr<termStructureParser>>::iterator it;
            it = list.find(SwapParser.getDiscountingTermStructureName());
            if (it != list.end()) {
                discountingTermStructure = it->second->getTermStructure();

            } else {
                erroros << SwapParser.getDiscountingTermStructureName() << " not found ";
                CROW_LOG_DEBUG << SwapParser.getDiscountingTermStructureName() << " not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }

            it = list.find(SwapParser.getForecastingTermStructureName());
            if (it != list.end()) {
                forecastingTermStructure = it->second->getTermStructure();

            } else {
                erroros << SwapParser.getForecastingTermStructureName() << " not found ";
                CROW_LOG_DEBUG << SwapParser.getForecastingTermStructureName() << " not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }
        }
    }

    if(!error) {

        try {

            SwapParser.setDiscountingTermStructure(discountingTermStructure);
            SwapParser.setForecastingTermStructure(forecastingTermStructure);

            SwapParser.createVanillaSwap();

        }catch (Error re) {

            error = true;
            erroros << "Error when creating bond: " << re.what();
            CROW_LOG_DEBUG << "Error when creating bond: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();

        }

    }

    boost::shared_ptr<swapPrice> sp {new swapPrice};

    if(!error){
        try {
            sp->NPV = SwapParser.getNPV();
        }catch (Error re) {
            error = true;
            erroros << "Error when getting swap NPV: " << re.what();
            CROW_LOG_DEBUG << "Error when getting swap NPV: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

        try {
            sp->fairSpread = SwapParser.getFairSpread();
        }catch (Error re) {
            error = true;
            erroros << "Error when getting swap fair spread: " << re.what();
            CROW_LOG_DEBUG << "Error when getting swap fair spread: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

        try {
            sp->fairRate = SwapParser.getFairRate();
        }catch (Error re) {
            error = true;
            erroros << "Error when getting swap fair rate: " << re.what();
            CROW_LOG_DEBUG << "Error when getting swap fair rate: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

    }

    if(!error){
        try {
            SwapParser.getFlows(sp);
        }catch (Error re) {
            error = true;
            erroros << "Error when getting swap flows: " << re.what();
            CROW_LOG_DEBUG << "Error when getting swap flows: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

    }

    if(error){
        (*response)["response"] = "ko";
        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["VanillaInterestRateSwapErrors"][i] = *it;
            i++;

        }
    }else{
        (*response)["response"] = "ok";
        (*response)["message"]["NPV"] =sp->NPV;
        (*response)["message"]["FairSpread"] =sp->fairSpread;
        (*response)["message"]["FairRate"] =sp->fairRate;
        (*response)["message"]["AccruedDays"] =sp->accruedDays;

        for (int i = 0; i < sp->fixedFlows.size(); i++) {

            std::ostringstream os;

            if(sp->fixedFlows[i]->flowType == Interest){
                (*response)["message"]["FixedFlows"][i]["Type"] = "Interest";
                (*response)["message"]["FixedFlows"][i]["Amount"] = sp->fixedFlows[i]->amount;

                os << QuantLib::io::iso_date(sp->fixedFlows[i]->accrualStartDate);
                (*response)["message"]["FixedFlows"][i]["AccrualStartDate"] = os.str();
                os.str("");
                os.clear();

                os << QuantLib::io::iso_date(sp->fixedFlows[i]->accrualEndDate);
                (*response)["message"]["FixedFlows"][i]["AccrualEndDate"] = os.str();

                (*response)["message"]["FixedFlows"][i]["Discount"] = sp->fixedFlows[i]->discount;
                (*response)["message"]["FixedFlows"][i]["Rate"] = sp->fixedFlows[i]->fixing;
                (*response)["message"]["FixedFlows"][i]["Price"] = sp->fixedFlows[i]->price;
            }else if(sp->fixedFlows[i]->flowType == PastInterest){

                (*response)["message"]["FixedFlows"][i]["Type"] = "PastInterest";

                (*response)["message"]["FixedFlows"][i]["Amount"] = sp->fixedFlows[i]->amount;

                os << QuantLib::io::iso_date(sp->fixedFlows[i]->accrualStartDate);
                (*response)["message"]["FixedFlows"][i]["AccrualStartDate"] = os.str();
                os.str("");
                os.clear();

                os << QuantLib::io::iso_date(sp->fixedFlows[i]->accrualEndDate);
                (*response)["message"]["FixedFlows"][i]["AccrualEndDate"] = os.str();

                (*response)["message"]["FixedFlows"][i]["Rate"] = sp->fixedFlows[i]->fixing;

            }

        }

        for (int i = 0; i < sp->floatingFlows.size(); i++) {

            std::ostringstream os;

            if(sp->floatingFlows[i]->flowType == Interest){
                (*response)["message"]["FloatingFlows"][i]["Type"] = "Interest";
                os << QuantLib::io::iso_date(sp->floatingFlows[i]->date);
                (*response)["message"]["FloatingFlows"][i]["FixingDate"] = os.str();
                os.str("");
                os.clear();

                (*response)["message"]["FloatingFlows"][i]["Amount"] = sp->floatingFlows[i]->amount;

                os << QuantLib::io::iso_date(sp->floatingFlows[i]->accrualStartDate);
                (*response)["message"]["FloatingFlows"][i]["AccrualStartDate"] = os.str();
                os.str("");
                os.clear();

                os << QuantLib::io::iso_date(sp->floatingFlows[i]->accrualEndDate);
                (*response)["message"]["FloatingFlows"][i]["AccrualEndDate"] = os.str();

                (*response)["message"]["FloatingFlows"][i]["Discount"] = sp->floatingFlows[i]->discount;
                (*response)["message"]["FloatingFlows"][i]["Rate"] = sp->floatingFlows[i]->fixing;
                (*response)["message"]["FloatingFlows"][i]["Price"] = sp->floatingFlows[i]->price;
            }else if(sp->floatingFlows[i]->flowType == PastInterest){

                (*response)["message"]["FloatingFlows"][i]["Type"] = "PastInterest";
                os << QuantLib::io::iso_date(sp->floatingFlows[i]->date);
                (*response)["message"]["FloatingFlows"][i]["FixingDate"] = os.str();
                os.str("");
                os.clear();

                (*response)["message"]["FloatingFlows"][i]["Amount"] = sp->floatingFlows[i]->amount;

                os << QuantLib::io::iso_date(sp->floatingFlows[i]->accrualStartDate);
                (*response)["message"]["FloatingFlows"][i]["AccrualStartDate"] = os.str();
                os.str("");
                os.clear();

                os << QuantLib::io::iso_date(sp->floatingFlows[i]->accrualEndDate);
                (*response)["message"]["FloatingFlows"][i]["AccrualEndDate"] = os.str();

                (*response)["message"]["FloatingFlows"][i]["Rate"] = sp->floatingFlows[i]->fixing;

            }
        }

    }

}
