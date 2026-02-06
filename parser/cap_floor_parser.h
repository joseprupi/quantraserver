#ifndef QUANTRASERVER_CAPFLOORPARSER_H
#define QUANTRASERVER_CAPFLOORPARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/capfloor.hpp>
#include <ql/cashflows/iborcoupon.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "cap_floor_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"
#include "index_registry.h"

using namespace QuantLib;

class CapFloorParser {
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    std::shared_ptr<QuantLib::CapFloor> parse(
        const quantra::CapFloor *capFloor,
        const quantra::IndexRegistry& indices);

    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_CAPFLOORPARSER_H
