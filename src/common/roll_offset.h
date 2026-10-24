#pragma once

#include <ql/time/date.hpp>

/// Scoped "roll offset" for market snapshots built at a rolled evaluation
/// date (e.g. the swaption rebump theta roll). The parsers consult it when
/// they resolve an EXPLICIT `reference_date` (curve, helper curve, vol
/// surface) so the whole market rolls consistently with the evaluation date:
/// `ref = reference_date + offset`. Date-anchored market data (zero/discount/
/// forward points, fixings) is NOT shifted -- those dates are data.
///
/// Request-scoped state, kept thread_local: a worker thread serves one
/// request at a time, the guard lives on its stack, and no other thread can
/// see or clear its offset (same model as the calendar overrides' record).
/// Zero outside a guard scope. QuantLib's evaluation date and fixings live in
/// QuantLib's own singletons (see eval_date_guard.h, fixings_guard.h).
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
