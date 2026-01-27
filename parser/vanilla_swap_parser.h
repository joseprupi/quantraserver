#ifndef QUANTRASERVER_VANILLA_SWAP_PARSER_H
#define QUANTRASERVER_VANILLA_SWAP_PARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/termstructures/yield/piecewiseyieldcurve.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/ibor/usdlibor.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>
#include <ql/time/daycounters/actualactual.hpp>

#include "vanilla_swap_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"
#include "index_parser.h"

using namespace QuantLib;
using namespace quantra;

/**
 * VanillaSwapParser - Parses Flatbuffers VanillaSwap into QuantLib VanillaSwap
 */
class VanillaSwapParser
{
private:
    // Term structure handles for index
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    /**
     * Parse a VanillaSwap from Flatbuffers to QuantLib
     * 
     * @param swap The Flatbuffers swap definition
     * @return A shared_ptr to the QuantLib VanillaSwap
     */
    std::shared_ptr<QuantLib::VanillaSwap> parse(const quantra::VanillaSwap *swap);

    /**
     * Link the forwarding term structure (needed for floating leg index)
     * 
     * @param term_structure The forwarding curve
     */
    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_VANILLA_SWAP_PARSER_H