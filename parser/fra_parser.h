#ifndef QUANTRASERVER_FRAPARSER_H
#define QUANTRASERVER_FRAPARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/forwardrateagreement.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "fra_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"
#include "index_registry.h"

using namespace QuantLib;

class FRAParser {
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;
    RelinkableHandle<YieldTermStructure> discounting_term_structure_;

public:
    std::shared_ptr<QuantLib::ForwardRateAgreement> parse(
        const quantra::FRA *fra,
        const quantra::IndexRegistry& indices);

    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
    void linkDiscountingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_FRAPARSER_H
