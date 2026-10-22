#ifndef QUANTRA_EQUITY_UNDERLYING_REGISTRY_H
#define QUANTRA_EQUITY_UNDERLYING_REGISTRY_H

#include "pricing_generated.h"

#include <ql/handle.hpp>
#include <ql/quote.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>
#include <ql/types.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace quantra {

struct PricingRegistry;

struct EquityUnderlyingRuntime {
    /// ISO 4217 code from `EquityUnderlyingSpec.currency`; empty when the spec
    /// carries none (the equity option endpoint does not need it; basket
    /// products that pay in a note currency require it).
    std::string currency;
    QuantLib::Handle<QuantLib::Quote> spot;
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend;
    /// Discrete cash dividends declared on the underlying spec (ex-dividend
    /// date + cash amount). Empty when the spec carries none. These are
    /// escrowed cash events priced alongside the continuous dividend-yield
    /// curve, not a substitute for it.
    std::vector<QuantLib::Date> discreteDividendDates;
    std::vector<QuantLib::Real> discreteDividendAmounts;
};

class EquityUnderlyingRegistryBuilder {
public:
    std::unordered_map<std::string, EquityUnderlyingRuntime> build(
        const quantra::EquityMarketData* equity, const PricingRegistry& reg) const;
};

} // namespace quantra

#endif // QUANTRA_EQUITY_UNDERLYING_REGISTRY_H
