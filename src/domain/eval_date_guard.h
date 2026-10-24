#ifndef QUANTRA_EVAL_DATE_GUARD_H
#define QUANTRA_EVAL_DATE_GUARD_H

#include <ql/settings.hpp>
#include <ql/time/date.hpp>

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
 */
struct EvalDateGuard {
    QuantLib::Date saved;
    EvalDateGuard() : saved(QuantLib::Settings::instance().evaluationDate()) {}
    ~EvalDateGuard() { QuantLib::Settings::instance().evaluationDate() = saved; }
    EvalDateGuard(const EvalDateGuard&) = delete;
    EvalDateGuard& operator=(const EvalDateGuard&) = delete;
};

} // namespace quantra

#endif // QUANTRA_EVAL_DATE_GUARD_H
