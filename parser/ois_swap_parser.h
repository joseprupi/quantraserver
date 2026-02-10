#ifndef QUANTRASERVER_OISSWAPPARSER_H
#define QUANTRASERVER_OISSWAPPARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/overnightindexedswap.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "ois_swap_generated.h"
#include "common_parser.h"
#include "enums.h"
#include "index_registry.h"

using namespace QuantLib;

class OisSwapParser {
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    std::shared_ptr<QuantLib::OvernightIndexedSwap> parse(
        const quantra::OisSwap *swap,
        const quantra::IndexRegistry& indices);

    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_OISSWAPPARSER_H
