#ifndef QUANTRA_REQUIRE_FIELD_H
#define QUANTRA_REQUIRE_FIELD_H

/**
 * Shared presence/validation for request fields declared optional on the
 * FlatBuffers wire (`field:double = null;`, `field:enums.Foo = null;`, a `Period`
 * sub-table, ...). A bare wire field defaults to 0 / the first enum / false when
 * omitted, so a forgotten value would silently price against that default; these
 * helpers turn an omitted (or non-finite / non-positive) value into a named 400
 * (QuantraInvalidArgument).
 *
 * This header is FlatBuffers-aware on purpose and is only ever included from the
 * parsers/mappers layer, which lift the validated plain value into the domain
 * structs. It must never be included from the FlatBuffers-free evaluators.
 *
 * String identity fields (ids, curve references) and the presence-or-quote_id
 * "exactly one of" rule are deliberately not covered here — they need a
 * different guard (empty-string / multi-field) than single-field presence.
 */

#include <cmath>
#include <string>

#include "flatbuffers/flatbuffers.h"
#include <ql/time/period.hpp>

#include "common_generated.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

/**
 * Require an optional scalar to be present and finite. For RATE-like values
 * (quote values, coupon rates, FRA/cap-floor strikes, inflation helper quotes)
 * that may legitimately be negative, so only a missing or non-finite value is
 * rejected — no positivity bound.
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
 * Require an optional scalar to be present, finite and strictly positive. For
 * notionals / face amounts / base nominals / redemptions / equity strike prices
 * / cash payouts / volatilities — quantities meaningless at or below zero.
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

/**
 * Require an optional enum to be present. Convention enums (calendar /
 * day_counter / business_day_convention / frequency / ...) are declared
 * `field:enums.Foo = null` so an omission is a named 400 rather than the silent
 * alphabetical-0 value.
 */
template <typename T>
inline T requireEnum(flatbuffers::Optional<T> v, const std::string& name) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    return v.value();
}

/**
 * Require an optional integer convention (fixing_days / settlement_days /
 * spot_days / cash_settlement_days) to be present — a bare int would silently
 * default to 0.
 */
inline int requireInt(flatbuffers::Optional<int> v, const std::string& name) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    return v.value();
}

/**
 * Require an optional bool convention (end_of_month, CDS ISDA flags, inflation
 * interpolated/revised) to be present — a bare bool would silently default to
 * false (or a hard-coded true).
 */
inline bool requireBool(flatbuffers::Optional<bool> v, const std::string& name) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    return v.value();
}

/**
 * Require a present `Period` sub-table and both of its fields, returning a QL
 * Period. A bare `n` would default to 0 and an omitted `unit` to Months, so a
 * forgotten value becomes a named 400 instead of a bogus (0 Months) tenor.
 */
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

#endif // QUANTRA_REQUIRE_FIELD_H
