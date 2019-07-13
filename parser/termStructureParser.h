//
// Created by Josep Rubio on 6/14/17.
//

#ifndef QUANTRASERVER_TERMSTRUCTUREPARSER_H
#define QUANTRASERVER_TERMSTRUCTUREPARSER_H

#include "crow.h"
#include "../common/enums.h"
#include "../common/common.h"
#include "rapidjson.h"
#include "QuantraParser.h"
#include <document.h>
#include "termStructurePointParser.h"
#include "zeroCurveSpreadParser.h"

class termStructureParser: public QuantraParser {

private:

    std::string message;

    string id;

    BootstrapTrait bootstrapTrait;
    InterpolationEnum interpolator;
    DayCounter dayCounter;

    //Flat term structure
    bool isFlat = false;
    Date flatDate;
    Rate flatRate;

    bool exogeneousDiscount = false;
    std::map<string,boost::shared_ptr<termStructureParser>> termStructuresList;

    bool interpolated = false;
    std::vector<Date> interpolatedDates;
    std::vector<Rate> interpolatedRates;

    Date asOfDate;

    std::vector<boost::shared_ptr<RateHelper> > rateHelperStructure;
    //std::vector<jump> jumps;
    std::vector<Handle<Quote>> jumps;
    std::vector<Date> jumpDates;

    boost::shared_ptr<YieldTermStructure> termStructure;
    boost::shared_ptr<YieldTermStructure> tmpTermStructure;
    //RelinkableHandle<YieldTermStructure> handleTermStructure;

    boost::shared_ptr<zeroCurveSpreadParser> ZeroCurveSpreadParser;
    bool hasSimpleSpread = false;
    bool hasInterpolatedSpread = false;

public:

    std::vector<std::pair<Date,Real>> nodes;

    termStructureParser(std::string message, std::vector<std::string> *errorLog,
                        std::map<string,boost::shared_ptr<termStructureParser>> termStructuresList);

    boost::shared_ptr<YieldTermStructure> createTermStructure();
    boost::shared_ptr<YieldTermStructure> getTermStructure();

    bool parse(Value &str);

    void setInterpolator(const InterpolationEnum &interpolator);
    void setDayCounter(const DayCounter &dayCounter);
    void setAsOfDate(const Date &asOfDate);

    string getId();

    DayCounter getDayCounter();

    bool SetPoint(boost::shared_ptr<termStructurePointParser> rp);
};


#endif //QUANTRASERVER_TERMSTRUCTUREPARSER_H
