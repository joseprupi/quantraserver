#ifndef QUANTRASERVER_SWAPTION_PARSER_H
#define QUANTRASERVER_SWAPTION_PARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <ql/exercise.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "swaption_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"
#include "vanilla_swap_parser.h"

using namespace QuantLib;

/**
 * SwaptionParser - Parses Flatbuffers Swaption into QuantLib Swaption
 */
class SwaptionParser
{
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    std::shared_ptr<QuantLib::Swaption> parse(const quantra::Swaption *swaption);
    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_SWAPTION_PARSER_H
