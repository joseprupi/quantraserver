#ifndef QUANTRA_REQUIRE_PERIOD_H
#define QUANTRA_REQUIRE_PERIOD_H

/**
 * Presence validation for the shared `Period` table (common.fbs). Both `n` and
 * `unit` are declared optional on the wire (`= null`); a bare `n` would default
 * to a silent 0 and an omitted `unit` would silently mean Months, so a forgotten
 * value turns into a named 400 (QuantraInvalidArgument) here instead of pricing
 * a bogus (0 Months) tenor. Only ever included from the parsers/mappers layer.
 */

#include <string>

#include <ql/time/period.hpp>

#include "common_generated.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

/// Require a present Period table and both of its fields, returning a QL Period.
inline QuantLib::Period requirePeriod(const quantra::Period* p,
                                      const std::string& name) {
    if (p == nullptr) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    if (!p->n().has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + ".n is required");
    }
    if (!p->unit().has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + ".unit is required");
    }
    return QuantLib::Period(p->n().value(), TimeUnitToQL(p->unit().value()));
}

} // namespace quantra

#endif // QUANTRA_REQUIRE_PERIOD_H
