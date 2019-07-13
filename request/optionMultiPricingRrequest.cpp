//
// Created by Josep Rubio on 6/26/17.
//

#include "optionMultiPricingRrequest.h"

void optionMultiPricingRrequest::processRequest(crow::request req, boost::shared_ptr<crow::json::wvalue> response) {

    auto x = crow::json::load(req.body);

    bool error = false;
    bool tmpBool = false;

    std::ostringstream erroros;

    (*response)["response"] = "ok";

    std::vector<std::string> *errorLog = new std::vector<std::string>();

    /*Temporary to solve "{\"\"}" issue*/
    if(req.body.at(0) == '"'){
        (*response)["response"] = "ko";
        (*response)["message"]["OptionPricingErrors"] = "Wrong format";
        return;
    }

    Document d;
    ParseResult ok = d.Parse(req.body.c_str());

    if (!ok) {
        fprintf(stderr, "JSON parse error: %s (%u)",
                GetParseError_En(ok.Code()), ok.Offset());

        erroros << "JSON parse error: " << GetParseError_En(ok.Code()) << " " << ok.Offset();

        (*response)["response"] = "ko";
        (*response)["message"]["OptionPricingErrors"] = erroros.str();

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

    optionParser OptionParserParser(errorLog, "Option");
    OptionParserParser.setMultioption(true);
    iterator = d.FindMember("Option");
    if (iterator != d.MemberEnd()) {
        Value &optionJSON = d["Option"];
        tmpBool = OptionParserParser.parse(optionJSON);
        error = error || tmpBool;
    } else {
        erroros << "Option: Not found ";
        CROW_LOG_DEBUG << "Option: Not found ";
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

    boost::shared_ptr<YieldTermStructure> riskFreeRateTermStructure;
    boost::shared_ptr<YieldTermStructure> dividendYieldTermStructure;

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
            it = list.find(OptionParserParser.getEuropeanMultiOptionParser()->getRiskFreeRateTermStructureName());
            if (it != list.end()) {
                riskFreeRateTermStructure = it->second->getTermStructure();

            } else {
                erroros << OptionParserParser.getEuropeanMultiOptionParser()->getRiskFreeRateTermStructureName()
                        << " not found ";
                CROW_LOG_DEBUG << OptionParserParser.getEuropeanMultiOptionParser()->getRiskFreeRateTermStructureName()
                               << " not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }

            it = list.find(OptionParserParser.getEuropeanMultiOptionParser()->getDividendYieldTermStructureName());
            if (it != list.end()) {
                dividendYieldTermStructure = it->second->getTermStructure();

            } else {
                erroros << OptionParserParser.getEuropeanMultiOptionParser()->getDividendYieldTermStructureName()
                        << " not found ";
                CROW_LOG_DEBUG << OptionParserParser.getEuropeanMultiOptionParser()->getDividendYieldTermStructureName()
                               << " not found ";
                errorLog->push_back(erroros.str());
                erroros.str("");
                erroros.clear();
                error = true;
            }
        }
    }

    if(!error){

        try{

            if(OptionParserParser.getOptionStyle() == EuropeanString) {

                VolTermStructureParser.setDate(PricingParser.getAsOfDate());
                Settings::instance().evaluationDate() = PricingParser.getAsOfDate();

                std::vector<Date> maturities = OptionParserParser.getEuropeanMultiOptionParser()->getMaturities();
                std::vector<Rate> underlyings = OptionParserParser.getEuropeanMultiOptionParser()->getUnderlyings();

                /*boost::shared_ptr<SimpleQuote> r(new SimpleQuote(0.0));
                RelinkableHandle<Quote> underlying_value(r);

                boost::shared_ptr<StrikedTypePayoff> payoff(
                        new PlainVanillaPayoff(OptionParserParser.getEuropeanMultiOptionParser()->getType(),
                                               OptionParserParser.getEuropeanMultiOptionParser()->getStrike()));

                boost::shared_ptr<BlackScholesMertonProcess> bsmProcess(
                        new BlackScholesMertonProcess(underlying_value,
                                                      Handle<YieldTermStructure>(dividendYieldTermStructure),
                                                      Handle<YieldTermStructure>(riskFreeRateTermStructure),
                                                      Handle<BlackVolTermStructure>(VolTermStructureParser.createVolTermStructure())));

                boost::shared_ptr<AnalyticEuropeanEngine> engine(new AnalyticEuropeanEngine(bsmProcess));*/

                int i = 0;
                for(Date maturity : maturities){
                    for(Rate underlying : underlyings){

                        boost::shared_ptr<SimpleQuote> r(new SimpleQuote(underlying));
                        RelinkableHandle<Quote> underlying_value(r);

                        boost::shared_ptr<StrikedTypePayoff> payoff(
                                new PlainVanillaPayoff(OptionParserParser.getEuropeanMultiOptionParser()->getType(),
                                                       OptionParserParser.getEuropeanMultiOptionParser()->getStrike()));

                        boost::shared_ptr<BlackScholesMertonProcess> bsmProcess(
                                new BlackScholesMertonProcess(underlying_value,
                                                              Handle<YieldTermStructure>(dividendYieldTermStructure),
                                                              Handle<YieldTermStructure>(riskFreeRateTermStructure),
                                                              Handle<BlackVolTermStructure>(VolTermStructureParser.createVolTermStructure())));

                        boost::shared_ptr<AnalyticEuropeanEngine> engine(new AnalyticEuropeanEngine(bsmProcess));

                        boost::shared_ptr <Exercise> europeanExercise(new EuropeanExercise(maturity));

                        VanillaOption europeanOption(payoff, europeanExercise);

                        europeanOption.setPricingEngine(boost::shared_ptr<PricingEngine>(engine));

                        std::ostringstream os;
                        os << QuantLib::io::iso_date(maturity);

                        CROW_LOG_DEBUG << maturity;
                        CROW_LOG_DEBUG << underlying;
                        CROW_LOG_DEBUG << europeanOption.NPV();

                        (*response)["message"][i]["Spot"] = underlying;
                        (*response)["message"][i]["Maturity"] = os.str();
                        (*response)["message"][i]["NPV"] = europeanOption.NPV();
                        (*response)["message"][i]["Delta"] = europeanOption.delta();
                        (*response)["message"][i]["Gamma"] = europeanOption.gamma();
                        (*response)["message"][i]["DividendRho"] = europeanOption.dividendRho();
                        (*response)["message"][i]["Rho"] = europeanOption.rho();
                        (*response)["message"][i]["Vega"] = europeanOption.vega();
                        (*response)["message"][i]["Theta"] = europeanOption.theta();
                        (*response)["message"][i]["StrikeSensitivity"] = europeanOption.strikeSensitivity();
                        (*response)["message"][i]["ThetaPerDay"] = europeanOption.thetaPerDay();
                        i++;
                    }
                }

                /*



                (*response)["message"]["NPV"] = europeanOption.NPV();
                (*response)["message"]["Delta"] = europeanOption.delta();
                (*response)["message"]["Gamma"] = europeanOption.gamma();
                (*response)["message"]["DividendRho"] = europeanOption.dividendRho();
                (*response)["message"]["Rho"] = europeanOption.rho();
                (*response)["message"]["Vega"] = europeanOption.vega();
                (*response)["message"]["Theta"] = europeanOption.theta();
                (*response)["message"]["StrikeSensitivity"] = europeanOption.strikeSensitivity();
                (*response)["message"]["ThetaPerDay"] = europeanOption.thetaPerDay();*/

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

            (*response)["message"]["Option Pricing Errors"][i] = *it;
            i++;

        }
    }
}