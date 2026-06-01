#ifndef QUANTRA_EQUITY_UNDERLYING_REGISTRY_H
#define QUANTRA_EQUITY_UNDERLYING_REGISTRY_H

#include <string>
#include <unordered_map>

#include <ql/handle.hpp>
#include <ql/quote.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "pricing_generated.h"

namespace quantra {

struct PricingRegistry;

struct EquityUnderlyingRuntime {
    QuantLib::Handle<QuantLib::Quote> spot;
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend;
};

class EquityUnderlyingRegistryBuilder {
public:
    std::unordered_map<std::string, EquityUnderlyingRuntime> build(
        const quantra::EquityMarketData* equity,
        const PricingRegistry& reg) const;
};

} // namespace quantra

#endif // QUANTRA_EQUITY_UNDERLYING_REGISTRY_H
