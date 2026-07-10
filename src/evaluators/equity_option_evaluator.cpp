#include "equity_option_evaluator.h"

#include <cmath>
#include <limits>
#include <memory>
#include <variant>

#include <ql/cashflows/dividend.hpp>
#include <ql/exercise.hpp>
#include <ql/handle.hpp>
#include <ql/instruments/dividendschedule.hpp>
#include <ql/instruments/oneassetoption.hpp>
#include <ql/instruments/payoffs.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/payoff.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/pricingengines/vanilla/analyticdigitalamericanengine.hpp>
#include <ql/pricingengines/vanilla/analyticdividendeuropeanengine.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/pricingengines/vanilla/binomialengine.hpp>
#include <ql/pricingengines/vanilla/fdblackscholesvanillaengine.hpp>
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
                                const PricingContext& ctx) {
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

    // Build the QL payoff from the wire-selected payoff kind. All three are
    // StrikedTypePayoffs so they slot into VanillaOption/BarrierOption alike.
    std::shared_ptr<QuantLib::StrikedTypePayoff> payoff;
    switch (trade.payoffKind) {
        case EquityPayoffKind::PlainVanilla:
            payoff = std::make_shared<QuantLib::PlainVanillaPayoff>(
                trade.optionType, trade.strike);
            break;
        case EquityPayoffKind::CashOrNothing:
            payoff = std::make_shared<QuantLib::CashOrNothingPayoff>(
                trade.optionType, trade.strike, trade.cash);
            break;
        case EquityPayoffKind::AssetOrNothing:
            payoff = std::make_shared<QuantLib::AssetOrNothingPayoff>(
                trade.optionType, trade.strike);
            break;
    }
    const bool isPlainVanilla = trade.payoffKind == EquityPayoffKind::PlainVanilla;

    // Exercise style, read back from the QL exercise object the mapper built.
    const QuantLib::Exercise::Type exerciseType = trade.exercise->type();
    // American/Bermudan exercise dates must fall after the evaluation date;
    // European expiries keep their existing (unvalidated) behaviour.
    if (exerciseType == QuantLib::Exercise::American) {
        if (trade.exercise->lastDate() <= ctx.asOf) {
            QUANTRA_INVALID_ARGUMENT(
                "American exercise end_date must be after the evaluation date");
        }
    } else if (exerciseType == QuantLib::Exercise::Bermudan) {
        for (const auto& d : trade.exercise->dates()) {
            if (d <= ctx.asOf) {
                QUANTRA_INVALID_ARGUMENT(
                    "Bermudan exercise_dates must all be after the evaluation date");
            }
        }
    }

    QuantLib::Handle<QuantLib::Quote> spot = underlying.spot;
    QuantLib::Handle<QuantLib::YieldTermStructure> riskFree(discountIt->second->currentLink());
    QuantLib::Handle<QuantLib::YieldTermStructure> dividend(underlying.dividend);
    QuantLib::Handle<QuantLib::BlackVolTermStructure> blackVol(volIt->second.handle);
    auto process = std::make_shared<QuantLib::BlackScholesMertonProcess>(
        spot, dividend, riskFree, blackVol);

    // Discrete cash dividends declared on the underlying spec, if any. These
    // are escrowed cash events layered on top of the continuous dividend-yield
    // curve already baked into the process. When present, the European option
    // is priced with QuantLib's dividend-aware analytic engine
    // (AnalyticDividendEuropeanEngine over a FixedDividend schedule); when
    // absent, the plain no-dividend engines are used unchanged.
    const bool hasDiscreteDividends = !underlying.discreteDividendDates.empty();
    QuantLib::DividendSchedule dividendSchedule;
    if (hasDiscreteDividends) {
        dividendSchedule = QuantLib::DividendVector(
            underlying.discreteDividendDates, underlying.discreteDividendAmounts);
    }

    std::shared_ptr<QuantLib::Instrument> instrument;
    std::shared_ptr<QuantLib::OneAssetOption> oneAssetOption;
    if (trade.hasBarrier) {
        if (!isPlainVanilla) {
            QUANTRA_INVALID_ARGUMENT("Equity barrier options require a plain-vanilla payoff");
        }
        if (exerciseType != QuantLib::Exercise::European) {
            QUANTRA_INVALID_ARGUMENT("Equity barrier options require a European exercise");
        }
        if (equityModel->model_type != EquityModelTypeKind::BlackScholesAnalytic) {
            QUANTRA_INVALID_ARGUMENT("Equity barrier option currently requires model_type=BlackScholesAnalytic");
        }
        if (hasDiscreteDividends) {
            QUANTRA_INVALID_ARGUMENT("Discrete cash dividends are not supported for equity barrier options");
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
        if (isPlainVanilla) {
            switch (exerciseType) {
                case QuantLib::Exercise::European:
                    switch (equityModel->model_type) {
                        case EquityModelTypeKind::BlackScholesAnalytic:
                            if (hasDiscreteDividends) {
                                vanilla->setPricingEngine(
                                    std::make_shared<QuantLib::AnalyticDividendEuropeanEngine>(
                                        process, dividendSchedule));
                            } else {
                                vanilla->setPricingEngine(
                                    std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
                            }
                            break;
                        case EquityModelTypeKind::BinomialCRR: {
                            if (hasDiscreteDividends) {
                                QUANTRA_INVALID_ARGUMENT("Discrete cash dividends currently require model_type=BlackScholesAnalytic");
                            }
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
                    break;
                case QuantLib::Exercise::American:
                case QuantLib::Exercise::Bermudan:
                    // American/Bermudan plain-vanilla exercise has no closed
                    // form — QuantLib's finite-difference Black-Scholes engine
                    // is the native pricer. It is Black-Scholes based, so it is
                    // gated behind model_type=BlackScholesAnalytic; a binomial
                    // request (which does not support Bermudan lattices) is
                    // rejected rather than silently re-routed.
                    if (equityModel->model_type != EquityModelTypeKind::BlackScholesAnalytic) {
                        QUANTRA_INVALID_ARGUMENT(
                            "American and Bermudan equity options require "
                            "model_type=BlackScholesAnalytic (finite-difference Black-Scholes)");
                    }
                    if (hasDiscreteDividends) {
                        QUANTRA_INVALID_ARGUMENT(
                            "Discrete cash dividends are not supported for "
                            "American or Bermudan equity options");
                    }
                    vanilla->setPricingEngine(
                        std::make_shared<QuantLib::FdBlackScholesVanillaEngine>(process));
                    break;
                default:
                    QUANTRA_INVALID_ARGUMENT("Unsupported equity exercise style");
            }
        } else {
            // Digital payoffs (cash-or-nothing / asset-or-nothing).
            if (equityModel->model_type != EquityModelTypeKind::BlackScholesAnalytic) {
                QUANTRA_INVALID_ARGUMENT(
                    "Digital equity payoffs require model_type=BlackScholesAnalytic");
            }
            if (hasDiscreteDividends) {
                QUANTRA_INVALID_ARGUMENT(
                    "Discrete cash dividends are not supported for digital equity payoffs");
            }
            switch (exerciseType) {
                case QuantLib::Exercise::European:
                    // AnalyticEuropeanEngine prices cash-/asset-or-nothing
                    // European digitals directly via the Black calculator.
                    vanilla->setPricingEngine(
                        std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
                    break;
                case QuantLib::Exercise::American:
                    // At-hit American digital: AnalyticDigitalAmericanEngine
                    // supports both cash-or-nothing and asset-or-nothing.
                    vanilla->setPricingEngine(
                        std::make_shared<QuantLib::AnalyticDigitalAmericanEngine>(process));
                    break;
                case QuantLib::Exercise::Bermudan:
                default:
                    QUANTRA_INVALID_ARGUMENT(
                        "Bermudan exercise is not supported for digital equity payoffs");
            }
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

EquityOptionResult EquityOptionEvaluator::evaluate(const EquityOptionInputs& inputs,
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
