#pragma once

#include <ql/time/date.hpp>

/// Scoped "roll offset" for market snapshots built at a rolled evaluation
/// date (e.g. the swaption rebump theta roll). The parsers consult it when
/// they resolve an EXPLICIT `reference_date` (curve, helper curve, vol
/// surface) so the whole market rolls consistently with the evaluation date:
/// `ref = reference_date + offset`. Date-anchored market data (zero/discount/
/// forward points, fixings) is NOT shifted -- those dates are data.
///
/// Process-level state, one request at a time per worker (same model as the
/// evaluation date and the calendar overrides). Zero outside a guard scope.
namespace quantra {

/// Currently active offset in calendar days (0 outside a RollOffsetGuard).
int activeRollOffsetDays();

/// Applies the active offset to an explicitly supplied reference date.
QuantLib::Date applyRollOffset(const QuantLib::Date& explicitReference);

/// RAII: sets the offset for the lifetime of the guard, restores 0 on exit.
struct RollOffsetGuard {
    explicit RollOffsetGuard(int days);
    ~RollOffsetGuard();
    RollOffsetGuard(const RollOffsetGuard&) = delete;
    RollOffsetGuard& operator=(const RollOffsetGuard&) = delete;
};

} // namespace quantra
