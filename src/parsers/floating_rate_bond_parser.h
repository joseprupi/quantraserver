#ifndef QUANTRASERVER_FLOATINGRATEBONDPARSER_H
#define QUANTRASERVER_FLOATINGRATEBONDPARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/bonds/floatingratebond.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "floating_rate_bond_generated.h"
#include "enum_convert.h"
#include "date_convert.h"
#include "schedule_parser.h"
#include "index_registry.h"

using namespace QuantLib;

class FloatingRateBondParser {
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    std::shared_ptr<QuantLib::FloatingRateBond> parse(
        const quantra::FloatingRateBond *bond,
        const quantra::IndexRegistry& indices);

    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_FLOATINGRATEBONDPARSER_H
