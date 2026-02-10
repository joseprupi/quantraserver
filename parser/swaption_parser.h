#ifndef QUANTRASERVER_SWAPTIONPARSER_H
#define QUANTRASERVER_SWAPTIONPARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "swaption_generated.h"
#include "vanilla_swap_parser.h"
#include "ois_swap_parser.h"
#include "enums.h"
#include "common.h"
#include "index_registry.h"

using namespace QuantLib;

class SwaptionParser {
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    std::shared_ptr<QuantLib::Swaption> parse(
        const quantra::Swaption *swaption,
        const quantra::IndexRegistry& indices);

    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_SWAPTIONPARSER_H
