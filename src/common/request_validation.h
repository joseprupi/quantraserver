#ifndef QUANTRA_REQUEST_VALIDATION_H
#define QUANTRA_REQUEST_VALIDATION_H

/**
 * Shared validation for incoming request fields, decoded from the FlatBuffers
 * wire in the parsers/mappers layer. A bare wire field defaults to 0 / the first
 * enum / false / an empty vector when omitted, so a forgotten value would
 * silently price against that default; these helpers turn an omitted (or
 * otherwise invalid) value into a named 400 (QuantraInvalidArgument) and return
 * the validated plain value for the caller to lift into a domain struct.
 *
 * This is the home for reusable request-field validation: single-field presence
 * (scalars / enums / ints / bools), the `Period` sub-table, and the per-period
 * notionals vector. It is FlatBuffers-aware on purpose and must only ever be
 * included from parsers/mappers, never from the FlatBuffers-free evaluators.
 *
 * Not covered here (they need a different check than single-field presence):
 * string identity fields, which also reject an empty string; and the
 * `price | rate | quote_id` "exactly one of" rule. Those keep their own guards.
 */

#include <cmath>
#include <cstddef>
#include <string>
#include <vector>

#include "flatbuffers/flatbuffers.h"
#include <ql/time/period.hpp>

#include "common_generated.h"
#include "enum_convert.h"
#include "error.h"

namespace quantra {

// ---------------------------------------------------------------------------
// Single-field presence guards
// ---------------------------------------------------------------------------

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
 * alphabetical-0 value. One template serves every enum type.
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
 * Require an optional integer day-count convention (payment_lag / lookback_days
 * / lockout_days) to be present AND non-negative — these feed QuantLib
 * `Natural` parameters, where a negative value would silently wrap into a huge
 * unsigned day count.
 */
inline int requireNonNegativeInt(flatbuffers::Optional<int> v,
                                 const std::string& name) {
    if (!v.has_value()) {
        QUANTRA_INVALID_ARGUMENT(name + " is required");
    }
    if (v.value() < 0) {
        QUANTRA_INVALID_ARGUMENT(name + " must be non-negative");
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

// ---------------------------------------------------------------------------
// Sub-table guards
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Vector guards
// ---------------------------------------------------------------------------

/**
 * Validate and lift an optional per-period notionals vector.
 *
 * Returns false and leaves `out` empty when the field is absent (fb == nullptr),
 * signalling the caller to keep its constant-notional path. When present the
 * vector must be non-empty, carry exactly one entry per coupon period
 * (schedule.size() - 1) and hold only strictly positive amounts; any violation
 * throws QuantraInvalidArgument (HTTP 400) naming the owning table via
 * `context` (e.g. "SwapFixedLeg", "FixedRateBond").
 */
inline bool parseOptionalNotionals(const flatbuffers::Vector<double>* fb,
                                   std::size_t periods,
                                   const std::string& context,
                                   std::vector<double>& out) {
    out.clear();
    if (fb == nullptr) {
        return false;
    }
    if (fb->size() == 0) {
        QUANTRA_INVALID_ARGUMENT(context + ".notionals must be non-empty when present");
    }
    if (fb->size() != periods) {
        QUANTRA_INVALID_ARGUMENT(
            context + ".notionals has " + std::to_string(fb->size()) +
            " entries but the schedule has " + std::to_string(periods) + " periods");
    }
    out.reserve(fb->size());
    for (flatbuffers::uoffset_t i = 0; i < fb->size(); ++i) {
        const double v = fb->Get(i);
        if (!(v > 0.0)) {
            QUANTRA_INVALID_ARGUMENT(context + ".notionals entries must be positive");
        }
        out.push_back(v);
    }
    return true;
}

/**
 * Reject a present notionals vector on a path that does not (yet) support
 * amortizing/step-up notionals. `context` describes that path for the 400
 * (e.g. "OisSwap fixed leg"). A null pointer (absent field) is a no-op.
 */
inline void rejectUnsupportedNotionals(const flatbuffers::Vector<double>* fb,
                                       const std::string& context) {
    if (fb != nullptr) {
        QUANTRA_INVALID_ARGUMENT(context + " does not support amortizing notionals yet");
    }
}

} // namespace quantra

#endif // QUANTRA_REQUEST_VALIDATION_H
