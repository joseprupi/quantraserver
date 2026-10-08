#ifndef QUANTRASERVER_CALLABLEFIXEDRATEBONDPARSER_H
#define QUANTRASERVER_CALLABLEFIXEDRATEBONDPARSER_H

#include "date_convert.h"
#include "enum_convert.h"
#include "schedule_parser.h"

#include "callable_fixed_rate_bond_generated.h"

#include <ql/experimental/callablebonds/callablebond.hpp>

#include <memory>

class CallableFixedRateBondParser {
public:
    // Builds a QuantLib::CallableFixedRateBond from the wire table, including
    // the validated CallabilitySchedule. All validation (presence of the
    // required conventions, non-empty and strictly increasing call schedule,
    // call dates after issue_date, positive clean prices, present callability
    // types) happens here so the FB-free evaluator only ever sees a fully
    // constructed QuantLib instrument.
    std::shared_ptr<QuantLib::CallableFixedRateBond> parse(
        const quantra::CallableFixedRateBond* bond);
};

#endif // QUANTRASERVER_CALLABLEFIXEDRATEBONDPARSER_H
