#ifndef QUANTRASERVER_SCHEDULE_PARSER_H
#define QUANTRASERVER_SCHEDULE_PARSER_H

/**
 * Schedule Parser
 *
 * Parses a FlatBuffers Schedule into a QuantLib Schedule.
 */

#include <memory>

#include <ql/time/schedule.hpp>

#include "schedule_generated.h"
#include "enum_convert.h"
#include "date_convert.h"

using namespace QuantLib;
using namespace quantra;

class ScheduleParser
{
public:
    std::shared_ptr<QuantLib::Schedule> parse(const quantra::Schedule *schedule);
};

#endif // QUANTRASERVER_SCHEDULE_PARSER_H
