#ifndef QUANTRASERVER_VANILLASWAPPARSER_H
#define QUANTRASERVER_VANILLASWAPPARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "vanilla_swap_generated.h"
#include "enum_convert.h"
#include "date_convert.h"
#include "schedule_parser.h"
#include "index_registry.h"

using namespace QuantLib;

class VanillaSwapParser {
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    std::shared_ptr<QuantLib::VanillaSwap> parse(
        const quantra::VanillaSwap *swap,
        const quantra::IndexRegistry& indices);

    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_VANILLASWAPPARSER_H
