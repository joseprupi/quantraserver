#ifndef QUANTRASERVER_VOLATILITY_PARSER_H
#define QUANTRASERVER_VOLATILITY_PARSER_H

#include <ql/qldefines.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/termstructures/volatility/optionlet/optionletvolatilitystructure.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual365fixed.hpp>

#include "volatility_generated.h"
#include "enums.h"
#include "common.h"

using namespace QuantLib;
using namespace quantra;

/**
 * VolatilityParser - Parses Flatbuffers volatility into QuantLib vol structures
 */
class VolatilityParser
{
public:
    std::shared_ptr<QuantLib::OptionletVolatilityStructure> parse(
        const quantra::VolatilityTermStructure *vol);
};

#endif // QUANTRASERVER_VOLATILITY_PARSER_H
