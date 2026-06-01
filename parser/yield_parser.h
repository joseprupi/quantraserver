#ifndef QUANTRASERVER_YIELD_PARSER_H
#define QUANTRASERVER_YIELD_PARSER_H

/**
 * Yield Parser
 *
 * Parses a FlatBuffers Yield specification into a YieldStruct for bond analytics.
 */

#include <memory>

#include <ql/time/daycounter.hpp>
#include <ql/compounding.hpp>
#include <ql/time/frequency.hpp>

#include "common_generated.h"
#include "enum_convert.h"

using namespace QuantLib;
using namespace quantra;

struct YieldStruct
{
    QuantLib::DayCounter day_counter;
    QuantLib::Compounding compounding;
    QuantLib::Frequency frequency;
};

class YieldParser
{
public:
    std::shared_ptr<YieldStruct> parse(const quantra::Yield *yield);
};

#endif // QUANTRASERVER_YIELD_PARSER_H
