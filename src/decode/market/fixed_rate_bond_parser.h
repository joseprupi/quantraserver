#ifndef QUANTRASERVER_FIXEDRATEBONDPARSER_H
#define QUANTRASERVER_FIXEDRATEBONDPARSER_H

#include "date_convert.h"
#include "enum_convert.h"
#include "request_validation.h"
#include "schedule_parser.h"

#include "fixed_rate_bond_generated.h"

#include <ql/indexes/ibor/eonia.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/instruments/bond.hpp>
#include <ql/instruments/bonds/amortizingfixedratebond.hpp>
#include <ql/instruments/bonds/fixedratebond.hpp>
#include <ql/math/interpolations/cubicinterpolation.hpp>
#include <ql/math/interpolations/loginterpolation.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/qldefines.hpp>
#include <ql/termstructures/yield/oisratehelper.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yield/ratehelpers.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/actualactual.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/imm.hpp>

class FixedRateBondParser {

private:
public:
    // Returns a plain QuantLib::Bond: a FixedRateBond for constant face amount,
    // or an AmortizingFixedRateBond when the request carries per-period notionals.
    std::shared_ptr<QuantLib::Bond> parse(const quantra::FixedRateBond* ts);
};

#endif //QUANTRASERVER_FIXEDRATEBONDPARSER_H