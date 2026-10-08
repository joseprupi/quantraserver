#ifndef QUANTRA_FIXINGS_GUARD_H
#define QUANTRA_FIXINGS_GUARD_H

#include <ql/indexes/indexmanager.hpp>

namespace quantra {

/**
 * FixingsGuard - RAII reset of QuantLib's process-global index fixing store.
 *
 * Every `addFixing` on an Ibor / overnight / swap / inflation index lands in
 * `QuantLib::IndexManager`, which is a singleton shared by every request the
 * worker ever serves. Without a reset, fixings supplied by one request stay
 * visible to the next: a request that omits a fixing it needs would silently
 * price with whatever an earlier request happened to supply for the same
 * index name. Clearing the whole store on entry (and again on exit) makes the
 * fixings in the request body the only fixings a request can see.
 *
 * Process-level state, one request at a time per worker (same model as the
 * evaluation date, the calendar overrides and the roll offset). Nothing
 * cached across requests depends on live fixings: the curve cache stores
 * frozen pillar values, and the calibration caches store fitted parameters.
 */
struct FixingsGuard {
    FixingsGuard() { QuantLib::IndexManager::instance().clearHistories(); }
    ~FixingsGuard() { QuantLib::IndexManager::instance().clearHistories(); }
    FixingsGuard(const FixingsGuard&) = delete;
    FixingsGuard& operator=(const FixingsGuard&) = delete;
};

} // namespace quantra

#endif // QUANTRA_FIXINGS_GUARD_H
