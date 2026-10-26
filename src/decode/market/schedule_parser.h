#ifndef QUANTRASERVER_SCHEDULE_PARSER_H
#define QUANTRASERVER_SCHEDULE_PARSER_H

/**
 * Schedule Parser
 *
 * Parses a FlatBuffers Schedule into a QuantLib Schedule.
 */

#include "date_convert.h"
#include "enum_convert.h"

#include "schedule_generated.h"

#include <ql/time/schedule.hpp>

#include <memory>

class ScheduleParser {
public:
    std::shared_ptr<QuantLib::Schedule> parse(const quantra::Schedule* schedule);
};

#endif // QUANTRASERVER_SCHEDULE_PARSER_H
