#ifndef QUANTRA_LEG_NOTIONALS_H
#define QUANTRA_LEG_NOTIONALS_H

/**
 * Shared validation for the optional per-period `notionals` vector added to the
 * swap-leg and bond tables. FlatBuffers vectors are naturally optional (a null
 * pointer means the field was absent), so presence alone selects the
 * amortizing/step-up path. This header stays FlatBuffers-aware on purpose: it is
 * only ever included from parsers/mappers (never from the FB-free evaluators),
 * which lift the validated plain std::vector<double> into the domain structs.
 */

#include <cstddef>
#include <string>
#include <vector>

#include "flatbuffers/flatbuffers.h"

#include "error.h"

namespace quantra {

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

#endif // QUANTRA_LEG_NOTIONALS_H
