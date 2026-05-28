#include "fra_pricer.h"

#include <iostream>
#include <memory>

#include <ql/handle.hpp>
#include <ql/indexes/iborindex.hpp>
#include <ql/instruments/forwardrateagreement.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "common.h"
#include "error.h"

namespace quantra {

namespace {

FraPerTrade priceTrade(const FraTrade& trade,
                       const PricingRegistry& reg,
                       const PricingContext& ctx) {
    (void)ctx;
    auto discIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }
    auto fwdIt = reg.rates.curves.find(trade.forwardingCurveId);
    if (fwdIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Forwarding curve not found: " + trade.forwardingCurveId);
    }
    if (!reg.rates.indices.has(trade.indexId)) {
        QUANTRA_NOT_FOUND("Index not found: " + trade.indexId);
    }

    const auto& discHandle = *discIt->second;
    const auto& fwdHandle = *fwdIt->second;

    // IndexRegistry::getIborWithCurve downcasts the registry entry to
    // IborIndex and raises if the requested index is not an IborIndex.
    auto iborIndex = reg.rates.indices.getIborWithCurve(
        trade.indexId,
        QuantLib::Handle<QuantLib::YieldTermStructure>(fwdHandle.currentLink()));

    auto fra = std::make_shared<QuantLib::ForwardRateAgreement>(
        iborIndex,
        trade.startDate,
        trade.maturityDate,
        trade.position,
        trade.strike,
        trade.notional,
        QuantLib::Handle<QuantLib::YieldTermStructure>(discHandle.currentLink()));

    FraPerTrade out;
    out.npv = fra->NPV();
    out.forwardRate = fra->forwardRate();
    out.spotValue = out.npv;
    out.settlementDate = DateToIso(trade.startDate);

    std::cout << "FRA NPV: " << out.npv
              << ", Forward Rate: " << out.forwardRate * 100 << "%" << std::endl;
    return out;
}

} // namespace

FraResult FraPricer::price(const FraInputs& inputs,
                           const PricingRegistry& reg,
                           const PricingContext& ctx) const {
    FraResult result;
    result.trades.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        result.trades.push_back(priceTrade(trade, reg, ctx));
    }
    return result;
}

} // namespace quantra
