//
// Created by Josep Rubio on 7/4/17.
//

#ifndef QUANTRASERVER_VANILLAINTERESTRATESWAPPARSER_H
#define QUANTRASERVER_VANILLAINTERESTRATESWAPPARSER_H

#include "crow.h"
#include <document.h>
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include "floatingLegParser.h"
#include "fixedLegParser.h"

class vanillaInterestRateSwapParser: QuantraParser  {

private:

    boost::shared_ptr<bool> error;
    std::string message;
    std::vector<std::string> *errorLog;

    string discountingTermStructureName;
    string forecastingTermStructureName;

    VanillaSwap::Type swapType;
    Real nominal;

    Integer lengthInYears;
    Date startDate;
    Calendar calendar;

    bool genericSchedule = false;

    shared_ptr<VanillaSwap> vanillaSwap;

    boost::shared_ptr<fixedLegParser> FixedLegParser;
    boost::shared_ptr<floatingLegParser> FloatingLegParser;

    boost::shared_ptr<YieldTermStructure> discountingTermStructure;
    RelinkableHandle<YieldTermStructure> termStructure;
    boost::shared_ptr<YieldTermStructure> forecastingTermStructure;

public:

    vanillaInterestRateSwapParser(std::vector<std::string> *errorLog, std::string message);
    bool parse(Value &str);

    shared_ptr<VanillaSwap> createVanillaSwap();

    void setDiscountingTermStructure(boost::shared_ptr<YieldTermStructure>);
    void setForecastingTermStructure(boost::shared_ptr<YieldTermStructure>);

    string getDiscountingTermStructureName();
    string getForecastingTermStructureName();

    Real getNPV();
    Rate getFairRate();
    Spread getFairSpread();
    void getFlows(boost::shared_ptr<swapPrice> swapPrice);
};


#endif //QUANTRASERVER_VANILLAINTERESTRATESWAPPARSER_H
