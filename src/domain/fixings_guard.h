#ifndef QUANTRA_FIXINGS_GUARD_H
#define QUANTRA_FIXINGS_GUARD_H

#include <ql/indexes/indexmanager.hpp>

#include <exception>
#include <iostream>

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
 * The store is QuantLib's own singleton (process-wide; per thread only when
 * QuantLib is built with QL_ENABLE_SESSIONS), so the model is one request at
 * a time per worker, as for the evaluation date (EvalDateGuard). The engine's
 * own request state (roll offset, calendar-override record) is thread_local.
 * Nothing cached across requests depends on live fixings: the curve cache
 * stores frozen pillar values, and the calibration caches store fitted
 * parameters.
 *
 * Clearing notifies the observers of every index with stored fixings and
 * QuantLib rethrows their failures. A destructor must not throw (that is
 * std::terminate), so a failed exit clear is logged; the next request's
 * guard clears again on entry, and fails that request if it cannot.
 */
struct FixingsGuard {
    FixingsGuard() { QuantLib::IndexManager::instance().clearHistories(); }
    ~FixingsGuard() {
        try {
            QuantLib::IndexManager::instance().clearHistories();
        } catch (const std::exception& e) {
            std::cerr << "[FixingsGuard] clearing the index fixings: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[FixingsGuard] clearing the index fixings: unknown error" << std::endl;
        }
    }
    FixingsGuard(const FixingsGuard&) = delete;
    FixingsGuard& operator=(const FixingsGuard&) = delete;
};

} // namespace quantra

#endif // QUANTRA_FIXINGS_GUARD_H
