#ifndef QUANTRA_REQUIRE_SCALAR_H
#define QUANTRA_REQUIRE_SCALAR_H

/**
 * Shared presence/finiteness validation for request scalars that are declared
 * optional on the FlatBuffers wire (`field:double = null;`). A bare double
 * defaults to 0 when omitted, so a forgotten value would silently price as
 * zero (or a first-enum) instead of erroring; these helpers turn an omitted or
 * non-finite value into a named 400 (QuantraInvalidArgument).
 *
 * This header stays FlatBuffers-aware on purpose: it is only ever included from
 * the parsers/mappers layer (never from the FB-free evaluators), which lift the
 * validated plain double into the domain structs.
 */

#include <cmath>
#include <string>

#include "flatbuffers/flatbuffers.h"

#include "error.h"

namespace quantra {

/**
 * Require a presence-optional scalar to be provided and finite. Used for
 * RATE-like values (quote values, coupon rates, FRA/cap-floor strikes,
 * inflation helper quotes) that may legitimately be negative in some markets,
 * so only a missing or non-finite value is rejected — no positivity bound.
 */
inline double requireFinite(flatbuffers::Optional<double> v, const std::string& name) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    if (!std::isfinite(v.value())) {
        QUANTRA_INVALID_ARGUMENT(name + " must be finite");
    }
    return v.value();
}

/**
 * Require a presence-optional scalar to be provided, finite and strictly
 * positive. Used for notionals / face amounts / base nominals / redemptions /
 * equity strikes (prices) / cash payouts / volatilities — quantities that are
 * meaningless at or below zero.
 */
inline double requirePositive(flatbuffers::Optional<double> v, const std::string& name) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    if (!std::isfinite(v.value())) {
        QUANTRA_INVALID_ARGUMENT(name + " must be finite");
    }
    if (!(v.value() > 0.0)) {
        QUANTRA_INVALID_ARGUMENT(name + " must be positive");
    }
    return v.value();
}

} // namespace quantra

#endif // QUANTRA_REQUIRE_SCALAR_H
