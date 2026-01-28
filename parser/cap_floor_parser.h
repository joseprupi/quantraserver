#ifndef QUANTRASERVER_CAP_FLOOR_PARSER_H
#define QUANTRASERVER_CAP_FLOOR_PARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/capfloor.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/cashflows/cashflows.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>

#include "cap_floor_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"
#include "index_parser.h"

using namespace QuantLib;
using namespace quantra;

/**
 * CapFloorParser - Parses Flatbuffers CapFloor into QuantLib Cap/Floor
 */
class CapFloorParser
{
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    std::shared_ptr<QuantLib::CapFloor> parse(const quantra::CapFloor *capFloor);
    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_CAP_FLOOR_PARSER_H
