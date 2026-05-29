#include "equity_option_pricer.h"

#include <cmath>
#include <limits>
#include <memory>
#include <variant>

#include <ql/handle.hpp>
#include <ql/instruments/oneassetoption.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/payoff.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/pricingengines/vanilla/binomialengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "error.h"
#include "model_domain.h"

namespace quantra {

namespace {

/// Match legacy safeGreek: swallow QL exceptions thrown by greek accessors
/// on engines/payoffs that don't implement them, and report NaN. Preserves
/// byte-for-byte equivalence with the inline implementation in
/// equity_option_pricing_request.cpp.
template <typename Fn>
double safeGreek(Fn&& fn) {
    try {
        return fn();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

EquityOptionPerTrade priceTrade(const EquityOptionTrade& trade,
                                const PricingRegistry& reg,
                                const PricingContext& /*ctx*/) {
    auto discountIt = reg.rates.curves.find(trade.discountingCurveId);
    if (discountIt == reg.rates.curves.end()) {
        QUANTRA_NOT_FOUND("Discounting curve not found: " + trade.discountingCurveId);
    }
    auto underlyingIt = reg.equity.equityUnderlyings.find(trade.underlyingId);
    if (underlyingIt == reg.equity.equityUnderlyings.end()) {
        QUANTRA_NOT_FOUND("Equity underlying not found: " + trade.underlyingId);
    }
    auto volIt = reg.volatility.blackVols.find(trade.volatilityId);
    if (volIt == reg.volatility.blackVols.end()) {
        QUANTRA_NOT_FOUND("Black vol not found: " + trade.volatilityId);
    }
    auto modelIt = reg.volatility.modelDomains.find(trade.modelId);
    if (modelIt == reg.volatility.modelDomains.end()) {
        QUANTRA_NOT_FOUND("Model not found: " + trade.modelId);
    }
    const auto* equityModel =
        std::get_if<EquityVanillaModelDomain>(&modelIt->second.payload);
    if (equityModel == nullptr) {
        QUANTRA_INVALID_ARGUMENT("Model '" + trade.modelId + "' is not an equity model");
    }

    const auto& underlying = underlyingIt->second;

    auto payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(trade.optionType, trade.strike);
    QuantLib::Handle<QuantLib::Quote> spot = underlying.spot;
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree(discountIt->second->currentLink());
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend(underlying.dividend);
    QuantLib::Handle<QuantLib::BlackVolTermStructure> blackVol(volIt->second.handle);
    auto process = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        spot, dividend, riskFree, blackVol);

    std::shared_ptr<QuantLib::Instrument> instrument;
    std::shared_ptr<QuantLib::OneAssetOption> oneAssetOption;
    if (trade.hasBarrier) {
        if (equityModel->model_type != EquityModelTypeKind::BlackScholesAnalytic) {
            QUANTRA_INVALID_ARGUMENT("Equity barrier option currently requires model_type=BlackScholesAnalytic");
        }
        auto barrierOption = std::make_shared<QuantLib::BarrierOption>(
            trade.barrierType,
            trade.barrierLevel,
            trade.rebate,
            payoff,
            trade.exercise);
        barrierOption->setPricingEngine(
            std::make_shared<QuantLib::AnalyticBarrierEngine>(process));
        instrument = barrierOption;
        oneAssetOption = barrierOption;
    } else {
        auto vanilla = std::make_shared<QuantLib::VanillaOption>(payoff, trade.exercise);
        switch (equityModel->model_type) {
            case EquityModelTypeKind::BlackScholesAnalytic:
                vanilla->setPricingEngine(
                    std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
                break;
            case EquityModelTypeKind::BinomialCRR: {
                int steps = equityModel->binomial_steps;
                if (steps <= 0) {
                    QUANTRA_INVALID_ARGUMENT("EquityVanillaModelSpec.binomial_steps must be > 0");
                }
                vanilla->setPricingEngine(
                    std::make_shared<
                        QuantLib::BinomialVanillaEngine<QuantLib::CoxRossRubinstein>>(
                        process, steps));
                break;
            }
            default:
                QUANTRA_INVALID_ARGUMENT("Unsupported EquityModelType");
        }
        instrument = vanilla;
        oneAssetOption = vanilla;
    }

    const double npv = instrument->NPV();
    const double quantity = trade.quantity;

    EquityOptionPerTrade out;
    out.tradeId = trade.tradeId;
    out.npv = npv * quantity;
    out.delta = safeGreek([&]() { return oneAssetOption->delta() * quantity; });
    out.gamma = safeGreek([&]() { return oneAssetOption->gamma() * quantity; });
    out.vega = safeGreek([&]() { return oneAssetOption->vega() * quantity; });
    out.theta = safeGreek([&]() { return oneAssetOption->theta() * quantity; });
    out.rho = safeGreek([&]() { return oneAssetOption->rho() * quantity; });

    out.impliedVolatility = std::numeric_limits<double>::quiet_NaN();
    if (!trade.hasBarrier) {
        if (auto vanilla = std::dynamic_pointer_cast<QuantLib::VanillaOption>(instrument)) {
            out.impliedVolatility =
                safeGreek([&]() { return vanilla->impliedVolatility(npv, process); });
        }
    }

    out.usedSpot = spot->value();
    out.usedStrike = trade.strike;
    out.usedSettlement = trade.settlement;
    return out;
}

} // namespace

EquityOptionResult EquityOptionPricer::price(const EquityOptionInputs& inputs,
                                             const PricingRegistry& reg,
                                             const PricingContext& ctx) const {
    EquityOptionResult result;
    result.trades.reserve(inputs.trades.size());
    for (const auto& trade : inputs.trades) {
        result.trades.push_back(priceTrade(trade, reg, ctx));
    }
    return result;
}

} // namespace quantra
