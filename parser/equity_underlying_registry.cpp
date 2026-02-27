#include "equity_underlying_registry.h"

#include "error.h"

namespace quantra {

std::unordered_map<std::string, EquityUnderlyingRuntime> EquityUnderlyingRegistryBuilder::build(
    const quantra::Pricing* pricing,
    const PricingRegistry& reg) const {
    std::unordered_map<std::string, EquityUnderlyingRuntime> out;
    if (!pricing || !pricing->equity_underlyings()) {
        return out;
    }

    for (const auto* u : *pricing->equity_underlyings()) {
        if (!u || !u->id()) {
            QUANTRA_ERROR("Pricing.equity_underlyings[].id is required");
        }
        if (!u->spot_quote_id() || !u->dividend_yield_curve_id()) {
            QUANTRA_ERROR("EquityUnderlyingSpec requires spot_quote_id and dividend_yield_curve_id");
        }

        auto divIt = reg.curves.find(u->dividend_yield_curve_id()->str());
        if (divIt == reg.curves.end()) {
            QUANTRA_ERROR("Dividend yield curve not found: " + u->dividend_yield_curve_id()->str());
        }

        EquityUnderlyingRuntime runtime;
        runtime.spot = reg.quoteRegistry.getHandle(u->spot_quote_id()->str());
        runtime.dividend = QuantLib::Handle<QuantLib::YieldTermStructure>(divIt->second->currentLink());
        out.emplace(u->id()->str(), runtime);
    }
    return out;
}

} // namespace quantra
