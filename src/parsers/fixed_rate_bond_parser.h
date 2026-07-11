#ifndef QUANTRASERVER_FIXEDRATEBONDPARSER_H
#define QUANTRASERVER_FIXEDRATEBONDPARSER_H

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

#include <ql/instruments/bond.hpp>
#include <ql/instruments/bonds/fixedratebond.hpp>
#include <ql/instruments/bonds/amortizingfixedratebond.hpp>

#include "fixed_rate_bond_generated.h"
#include "enum_convert.h"
#include "date_convert.h"
#include "schedule_parser.h"
#include "leg_notionals.h"

using namespace QuantLib;
using namespace quantra;

class FixedRateBondParser
{

private:
public:
    // Returns a plain QuantLib::Bond: a FixedRateBond for constant face amount,
    // or an AmortizingFixedRateBond when the request carries per-period notionals.
    std::shared_ptr<QuantLib::Bond> parse(const quantra::FixedRateBond *ts);
};

#endif //QUANTRASERVER_FIXEDRATEBONDPARSER_H