#ifndef QUANTRASERVER_FRA_PARSER_H
#define QUANTRASERVER_FRA_PARSER_H

#include <ql/qldefines.hpp>
#include <ql/instruments/forwardrateagreement.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>

#include "fra_generated.h"
#include "enums.h"
#include "common.h"
#include "common_parser.h"
#include "index_parser.h"

using namespace QuantLib;
using namespace quantra;

/**
 * FRAParser - Parses Flatbuffers FRA into QuantLib ForwardRateAgreement
 */
class FRAParser
{
private:
    RelinkableHandle<YieldTermStructure> forwarding_term_structure_;

public:
    std::shared_ptr<QuantLib::ForwardRateAgreement> parse(const quantra::FRA *fra);
    void linkForwardingTermStructure(std::shared_ptr<YieldTermStructure> term_structure);
};

#endif // QUANTRASERVER_FRA_PARSER_H
