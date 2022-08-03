#ifndef QUANTRASERVER_COMMONPARSER_H
#define QUANTRASERVER_COMMONPARSER_H

#include <ql/qldefines.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/indexes/ibor/eonia.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/imm.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>

#include "common_generated.h"
#include "enums.h"
#include "common.h"
#include "schedule_generated.h"

using namespace QuantLib;
using namespace quantra;

struct YieldStruct
{
    QuantLib::DayCounter day_counter;
    QuantLib::Compounding compounding;
    QuantLib::Frequency frequency;
};

struct PricingStruct
{
    std::string as_of_date;
    std::string settlement_date;
    const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>> *curves;
    bool bond_pricing_details;
    bool bond_pricing_flows;
    const flatbuffers::Vector<flatbuffers::Offset<quantra::CouponPricer>> *coupon_pricers;
};

class ScheduleParser
{

private:
public:
    std::shared_ptr<QuantLib::Schedule> parse(const quantra::Schedule *schedule);
};

class YieldParser
{

private:
public:
    std::shared_ptr<YieldStruct> parse(const quantra::Yield *yield);
};

class PricingParser
{

private:
    std::map<std::string, std::shared_ptr<YieldTermStructure>> term_structures;

public:
    std::shared_ptr<PricingStruct> parse(const quantra::Pricing *pricing);
    std::shared_ptr<PricingStruct> parse_term_structure(const quantra::Pricing *pricing);
};

#endif // QUANTRASERVER_COMMONPARSER_H