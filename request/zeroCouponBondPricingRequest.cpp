//
// Created by Josep Rubio on 6/26/17.
//

#include "zeroCouponBondPricingRequest.h"
#include <document.h>

void zeroCouponBondPricingRequest::processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response){

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["ZeroCouponBondErrors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["ZeroCouponBondErrors"] = erroros.str();

        return;
    }

    zeroCouponBondParser BondParser(errorLog, "ZeroCouponBond");
    Value::MemberIterator iterator;
    iterator = d.FindMember("ZeroCouponBond");
    if(iterator != d.MemberEnd()){
        Value& bondJSON = d["ZeroCouponBond"];
        tmpBool = BondParser.parse(bondJSON);
        error = error || tmpBool;
    }else{
        erroros << "ZeroCouponBond: Not found ";
        CROW_LOG_DEBUG << "ZeroCouponBond: Not found ";
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
            std::map<string,boost::shared_ptr<termStructureParser>> list =
                    TermStructureListParser.getTermStructuresList();

            std::map<string,boost::shared_ptr<termStructureParser>>::iterator it;
            it =list.find(BondParser.getDiscountingTermStructureName());

            if (it != list.end()) {
                discountingTermStructure = it->second->getTermStructure();

            }else {
                erroros << BondParser.getDiscountingTermStructureName() << " not found ";
                CROW_LOG_DEBUG << BondParser.getDiscountingTermStructureName() << " not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }
        }
    }

    if(!error) {

        try {

            BondParser.setDiscountingTermStructure(discountingTermStructure);
            BondParser.createZeroCouponBond();

        }catch (Error re) {

            error = true;
            erroros << "Error when creating bond: " << re.what();
            CROW_LOG_DEBUG << "Error when creating bond: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();

        }

    }

    boost::shared_ptr<bondPrice> sp {new bondPrice};

    if(!error){
        try {
            sp->NPV = BondParser.getNPV();
        }catch (Error re) {
            error = true;
            erroros << "Error when getting bond NPV: " << re.what();
            CROW_LOG_DEBUG << "Error when getting bond NPV: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

        try {
            sp->cleanPrice = BondParser.getCleanPrice();
        }catch (Error re) {
            error = true;
            erroros << "Error when getting bond clean price: " << re.what();
            CROW_LOG_DEBUG << "Error when getting bond clean price: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

        try {
            sp->dirtyPrice = BondParser.getDirtyPrice();
        }catch (Error re) {
            error = true;
            erroros << "Error when getting bond dirty price: " << re.what();
            CROW_LOG_DEBUG << "Error when getting bond dirty price: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

        try {
            sp->accruedAmount = BondParser.getAccruedAmount();
        }catch (Error re) {
            error = true;
            erroros << "Error when getting bond dirty price: " << re.what();
            CROW_LOG_DEBUG << "Error when getting bond dirty price: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

        try {
            sp->yield = BondParser.getYield();
        }catch (Error re) {
            error = true;
            erroros << "Error when getting bond yield: " << re.what();
            CROW_LOG_DEBUG << "Error when getting bond yield: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

    }

    if(!error){
        try {
            BondParser.getFlows(sp);
        }catch (Error re) {
            error = true;
            erroros << "Error when getting bond flows: " << re.what();
            CROW_LOG_DEBUG << "Error when getting bond flows: " << re.what();
            errorLog->push_back(erroros.str());
            erroros.str("");
            erroros.clear();
        }

    }

    if(error) {
        (*response)["response"] = "ko";

        int i = 0;
        for (std::vector<string>::iterator it = errorLog->begin(); it != errorLog->end(); ++it) {

            (*response)["message"]["ZeroCouponBondErrors"][i] = *it;
            i++;

        }
    }else{
        (*response)["response"] = "ok";
        (*response)["message"]["NPV"] =sp->NPV;
        (*response)["message"]["CleanPrice"] =sp->cleanPrice;
        (*response)["message"]["DirtyPrice"] =sp->dirtyPrice;
        (*response)["message"]["Yield"] =sp->yield;

        for (int i = 0; i < sp->flows.size(); i++) {

            std::ostringstream os;

            os << QuantLib::io::iso_date(sp->flows[i]->date);
            (*response)["message"]["Flows"][i]["Date"] = os.str();
            os.str("");
            os.clear();

            (*response)["message"]["Flows"][i]["Amount"] = sp->flows[i]->amount;
            (*response)["message"]["Flows"][i]["Discount"] = sp->flows[i]->discount;
            (*response)["message"]["Flows"][i]["Price"] = sp->flows[i]->price;

        }

    }

}
