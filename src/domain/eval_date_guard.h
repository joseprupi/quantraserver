#ifndef QUANTRA_EVAL_DATE_GUARD_H
#define QUANTRA_EVAL_DATE_GUARD_H

#include <ql/settings.hpp>
#include <ql/time/date.hpp>

namespace quantra {

/**
 * EvalDateGuard - RAII guard for QuantLib's global evaluation date (this is 
 * needed for QuantLib to work correctly).
 */
struct EvalDateGuard {
    QuantLib::Date saved;
    EvalDateGuard() : saved(QuantLib::Settings::instance().evaluationDate()) {}
    ~EvalDateGuard() { QuantLib::Settings::instance().evaluationDate() = saved; }
};

} // namespace quantra

#endif // QUANTRA_EVAL_DATE_GUARD_H
