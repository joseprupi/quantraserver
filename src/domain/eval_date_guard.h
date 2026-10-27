#ifndef QUANTRA_EVAL_DATE_GUARD_H
#define QUANTRA_EVAL_DATE_GUARD_H

#include <ql/settings.hpp>
#include <ql/time/date.hpp>

#include <exception>
#include <iostream>

namespace quantra {

/**
 * EvalDateGuard - RAII guard for QuantLib's global evaluation date: saves
 * Settings::instance().evaluationDate() on entry and restores it on exit, so
 * the date a request sets never leaks into the next one.
 *
 * Settings is QuantLib's own singleton (process-wide; per thread only when
 * QuantLib is built with QL_ENABLE_SESSIONS), hence the model of one request
 * at a time per worker, shared with FixingsGuard. The engine's own request
 * state (roll offset, calendar-override record) is thread_local.
 *
 * As QuantLib's SavedSettings does, it saves the stored value, so an unset
 * date ("today") is restored unset rather than pinned to the current day.
 * Restoring notifies the date's observers and QuantLib rethrows their
 * failures once the date is stored. A destructor must not throw (that is
 * std::terminate), so such a failure is logged instead.
 */
struct EvalDateGuard {
    QuantLib::Date saved;
    EvalDateGuard() : saved(QuantLib::Settings::instance().evaluationDate().value()) {}
    ~EvalDateGuard() {
        try {
            QuantLib::Settings::instance().evaluationDate() = saved;
        } catch (const std::exception& e) {
            std::cerr << "[EvalDateGuard] restoring the evaluation date: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[EvalDateGuard] restoring the evaluation date: unknown error"
                      << std::endl;
        }
    }
    EvalDateGuard(const EvalDateGuard&) = delete;
    EvalDateGuard& operator=(const EvalDateGuard&) = delete;
};

} // namespace quantra

#endif // QUANTRA_EVAL_DATE_GUARD_H
