#ifndef QUANTRA_PRICING_CONTEXT_H
#define QUANTRA_PRICING_CONTEXT_H

#include <ql/settings.hpp>
#include <ql/time/date.hpp>

#include "common.h"
#include "pricing_registry.h"
#include "pricing_generated.h"

namespace quantra {

/**
 * PricingContext - per-request ambient values a pricer legitimately needs that
 * are not market data: pricers may consume this directly alongside the PricingRegistry.
 *
 *   asOf       - the evaluation date already set during registry build.
 *   settlement - optional, product-dependent settlement date.
 *   options    - PricingRequestOptions carried through from the registry.
 */
struct PricingContext {
    QuantLib::Date asOf;
    QuantLib::Date settlement;
    PricingRequestOptions options;
};

/**
 * makeContext - glue that assembles a PricingContext from the request's Pricing
 * block and the already-built registry. This is glue, not a pricer, so it MAY
 * touch generated headers.
 *
 *   asOf       = QuantLib::Settings::instance().evaluationDate() (set by build).
 *   settlement = pricing->settlement_date() if present, via the same DateToQL
 *                path the existing handlers use (null-guarded; left as a default
 *                Date() when absent).
 *   options    = reg.options (pass-through).
 */
inline PricingContext makeContext(const quantra::Pricing* pricing,
                                  const PricingRegistry& reg) {
    PricingContext ctx;
    ctx.asOf = QuantLib::Settings::instance().evaluationDate();
    if (pricing && pricing->settlement_date()) {
        ctx.settlement = DateToQL(pricing->settlement_date()->str());
    }
    ctx.options = reg.options;
    return ctx;
}

} // namespace quantra

#endif // QUANTRA_PRICING_CONTEXT_H
