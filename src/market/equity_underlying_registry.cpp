#include "equity_underlying_registry.h"

#include "date_convert.h"
#include "error.h"
#include "pricing_registry.h"

namespace quantra {

std::unordered_map<std::string, EquityUnderlyingRuntime> EquityUnderlyingRegistryBuilder::build(
    const quantra::EquityMarketData* equity,
    const PricingRegistry& reg) const {
    std::unordered_map<std::string, EquityUnderlyingRuntime> out;
    if (!equity || !equity->equity_underlyings()) {
        return out;
    }

    for (const auto* u : *equity->equity_underlyings()) {
        if (!u || !u->id()) {
            QUANTRA_INVALID_ARGUMENT("pricing.equity.equity_underlyings[].id is required");
        }
        if (!u->spot_quote_id() || !u->dividend_yield_curve_id()) {
            QUANTRA_INVALID_ARGUMENT("EquityUnderlyingSpec requires spot_quote_id and dividend_yield_curve_id");
        }
        if (out.count(u->id()->str()) != 0) {
            QUANTRA_INVALID_ARGUMENT("duplicate equity underlying id: " + u->id()->str());
        }

        auto divIt = reg.rates.curves.find(u->dividend_yield_curve_id()->str());
        if (divIt == reg.rates.curves.end()) {
            QUANTRA_NOT_FOUND("Dividend yield curve not found: " + u->dividend_yield_curve_id()->str());
        }

        EquityUnderlyingRuntime runtime;
        runtime.spot = reg.quoteRegistry.getHandle(u->spot_quote_id()->str());
        runtime.dividend = QuantLib::Handle<QuantLib::YieldTermStructure>(divIt->second->currentLink());

        // Discrete cash dividends: ex-dividend date + cash amount. Carried as
        // escrowed cash events priced alongside the continuous dividend curve.
        if (u->discrete_dividends()) {
            for (const auto* d : *u->discrete_dividends()) {
                if (!d || !d->ex_date()) {
                    QUANTRA_INVALID_ARGUMENT(
                        "EquityUnderlyingSpec.discrete_dividends[].ex_date is required");
                }
                runtime.discreteDividendDates.push_back(DateToQL(d->ex_date()->str()));
                runtime.discreteDividendAmounts.push_back(d->amount());
            }
        }

        out.emplace(u->id()->str(), runtime);
    }
    return out;
}

} // namespace quantra
