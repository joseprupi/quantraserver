#ifndef QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H
#define QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H

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

#include "term_structure_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"

using namespace QuantLib;
using namespace quantra;

class TermStructurePointParser
{

private:
public:
    TermStructurePointParser(){};
    std::shared_ptr<QuantLib::RateHelper> parse(uint8_t point_type, const void *point);
};

#endif //QUANTRASERVER_TERMSTRUCTUREPOINTPARSER_H