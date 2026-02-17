#ifndef QUANTRASERVER_BASISSWAPPARSER_H
#define QUANTRASERVER_BASISSWAPPARSER_H

#include <ql/qldefines.hpp>
#include <ql/instrument.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "basis_swap_generated.h"
#include "common_parser.h"
#include "enums.h"
#include "index_registry.h"

using namespace QuantLib;

class BasisSwapParser {
private:
    RelinkableHandle<YieldTermStructure> forwarding_leg1_;
    RelinkableHandle<YieldTermStructure> forwarding_leg2_;

public:
    std::shared_ptr<QuantLib::Swap> parse(
        const quantra::BasisSwap* swap,
        const quantra::IndexRegistry& indices);

    void linkForwardingTermStructures(
        std::shared_ptr<YieldTermStructure> leg1,
        std::shared_ptr<YieldTermStructure> leg2);
};

#endif // QUANTRASERVER_BASISSWAPPARSER_H
