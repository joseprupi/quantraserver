#include "equity_option_pricing_request.h"

#include <cmath>
#include <limits>

#include <ql/instruments/barrieroption.hpp>
#include <ql/instruments/asianoption.hpp>
#include <ql/instruments/oneassetoption.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/payoff.hpp>
#include <ql/pricingengines/asian/analytic_discr_geom_av_price.hpp>
#include <ql/pricingengines/barrier/analyticbarrierengine.hpp>
#include <ql/pricingengines/barrier/binomialbarrierengine.hpp>
#include <ql/pricingengines/vanilla/analyticeuropeanengine.hpp>
#include <ql/pricingengines/vanilla/binomialengine.hpp>
#include <ql/processes/blackscholesprocess.hpp>
#include <ql/settings.hpp>

#include "equity_model_parser.h"
#include "equity_option_parser.h"
#include "equity_underlying_registry.h"
#include "error.h"
#include "pricing_registry.h"

using namespace QuantLib;
using namespace quantra;

namespace {

template <typename Fn>
double safeGreek(Fn&& fn) {
    try {
        return fn();
    } catch (...) {
        return std::numeric_limits<double>::quiet_NaN();
    }
}

} // namespace

flatbuffers::Offset<PriceEquityOptionResponse> EquityOptionPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceEquityOptionRequest* request) const {
    if (!request || !request->pricing()) {
        QUANTRA_ERROR("PriceEquityOptionRequest.pricing is required");
    }

    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());
    EquityUnderlyingRegistryBuilder underlyingRegistryBuilder;
    auto underlyings = underlyingRegistryBuilder.build(request->pricing(), reg);
    EquityOptionParser optionParser;
    EquityModelParser modelParser;

    if (!request->options()) {
        QUANTRA_ERROR("PriceEquityOptionRequest.options is required");
    }

    std::vector<flatbuffers::Offset<EquityOptionResponse>> out;
    for (const auto* p : *request->options()) {
        if (!p || !p->option()) {
            QUANTRA_ERROR("PriceEquityOption.option is required");
        }
        if (!p->discounting_curve() || !p->volatility() || !p->model()) {
            QUANTRA_ERROR("PriceEquityOption requires discounting_curve, volatility, and model");
        }

        ParsedEquityOption opt = optionParser.parse(p->option());
        auto uIt = underlyings.find(opt.underlyingId);
        if (uIt == underlyings.end()) {
            QUANTRA_ERROR("Equity underlying not found: " + opt.underlyingId);
        }

        auto dIt = reg.curves.find(p->discounting_curve()->str());
        if (dIt == reg.curves.end()) {
            QUANTRA_ERROR("Discounting curve not found: " + p->discounting_curve()->str());
        }
        auto vIt = reg.blackVols.find(p->volatility()->str());
        if (vIt == reg.blackVols.end()) {
            QUANTRA_ERROR("Black vol not found: " + p->volatility()->str());
        }
        auto mIt = reg.models.find(p->model()->str());
        if (mIt == reg.models.end()) {
            QUANTRA_ERROR("Model not found: " + p->model()->str());
        }
        const auto* model = modelParser.parse(mIt->second, p->model()->str());
        const auto& underlying = uIt->second;

        if (!opt.payoff) {
            QUANTRA_ERROR("EquityOption payoff is required");
        }
        auto payoff = opt.payoff;
        auto spot = underlying.spot;
        Handle<YieldTermStructure> riskFree(dIt->second->currentLink());
        Handle<YieldTermStructure> dividend(underlying.dividend);
        Handle<BlackVolTermStructure> blackVol(vIt->second.handle);
        auto process = std::make_shared<BlackScholesMertonProcess>(spot, dividend, riskFree, blackVol);

        std::shared_ptr<QuantLib::Instrument> instrument;
        std::shared_ptr<QuantLib::OneAssetOption> oneAssetOption;
        if (opt.hasAsian) {
            if (dynamic_cast<QuantLib::EuropeanExercise*>(opt.exercise.get()) == nullptr) {
                QUANTRA_ERROR("Asian option currently supports only EquityEuropeanExercise");
            }
            if (dynamic_cast<QuantLib::PlainVanillaPayoff*>(payoff.get()) == nullptr) {
                QUANTRA_ERROR("Asian option currently supports only EquityPlainVanillaPayoff");
            }
            auto asian = std::make_shared<QuantLib::DiscreteAveragingAsianOption>(
                opt.asianAverageType,
                opt.asianRunningAccumulator,
                opt.asianPastFixings,
                opt.asianFixingDates,
                payoff,
                opt.exercise);
            if (model->model_type() != quantra::enums::EquityModelType_BlackScholesAnalytic) {
                QUANTRA_ERROR("Asian option currently supports only BlackScholesAnalytic model");
            }
            if (opt.asianAverageType != QuantLib::Average::Geometric) {
                QUANTRA_ERROR("Asian option currently supports only Geometric averaging");
            }
            asian->setPricingEngine(std::make_shared<QuantLib::AnalyticDiscreteGeometricAveragePriceAsianEngine>(process));
            instrument = asian;
            oneAssetOption = asian;
        } else if (opt.hasBarrier) {
            auto barrierOption = std::make_shared<QuantLib::BarrierOption>(
                opt.barrierType,
                opt.barrierLevel,
                opt.rebate,
                payoff,
                opt.exercise);
            if (model->model_type() == quantra::enums::EquityModelType_BlackScholesAnalytic) {
                // Analytic barrier engine is intended for European-style exercise.
                if (dynamic_cast<QuantLib::EuropeanExercise*>(opt.exercise.get()) == nullptr) {
                    QUANTRA_ERROR("AnalyticBarrierEngine requires EquityEuropeanExercise. Use BinomialCRR for non-European barrier exercise.");
                }
                barrierOption->setPricingEngine(std::make_shared<QuantLib::AnalyticBarrierEngine>(process));
            } else if (model->model_type() == quantra::enums::EquityModelType_BinomialCRR) {
                int steps = model->binomial_steps();
                if (steps <= 0) {
                    QUANTRA_ERROR("EquityVanillaModelSpec.binomial_steps must be > 0");
                }
                barrierOption->setPricingEngine(
                    std::make_shared<QuantLib::BinomialBarrierEngine<QuantLib::CoxRossRubinstein>>(process, steps));
            } else {
                QUANTRA_ERROR("Unsupported EquityModelType");
            }
            instrument = barrierOption;
            oneAssetOption = barrierOption;
        } else {
            auto vanilla = std::make_shared<QuantLib::VanillaOption>(payoff, opt.exercise);
            if (model->model_type() == quantra::enums::EquityModelType_BlackScholesAnalytic) {
                // Restrict analytic engine to European plain-vanilla payoff.
                if (dynamic_cast<QuantLib::EuropeanExercise*>(opt.exercise.get()) == nullptr) {
                    QUANTRA_ERROR("BlackScholesAnalytic currently supports only EquityEuropeanExercise. Use BinomialCRR for American/Bermudan.");
                }
                if (dynamic_cast<QuantLib::PlainVanillaPayoff*>(payoff.get()) == nullptr) {
                    QUANTRA_ERROR("BlackScholesAnalytic currently supports only EquityPlainVanillaPayoff. Use BinomialCRR for digital payoffs.");
                }
                vanilla->setPricingEngine(std::make_shared<QuantLib::AnalyticEuropeanEngine>(process));
            } else if (model->model_type() == quantra::enums::EquityModelType_BinomialCRR) {
                int steps = model->binomial_steps();
                if (steps <= 0) {
                    QUANTRA_ERROR("EquityVanillaModelSpec.binomial_steps must be > 0");
                }
                vanilla->setPricingEngine(
                    std::make_shared<QuantLib::BinomialVanillaEngine<QuantLib::CoxRossRubinstein>>(process, steps));
            } else {
                QUANTRA_ERROR("Unsupported EquityModelType");
            }
            instrument = vanilla;
            oneAssetOption = vanilla;
        }

        const double npv = instrument->NPV();
        const double quantity = opt.quantity;
        const double scaledNpv = npv * quantity;
        const double usedSpot = spot->value();
        const double usedStrike = opt.strike;

        const double delta = safeGreek([&]() { return oneAssetOption->delta() * quantity; });
        const double gamma = safeGreek([&]() { return oneAssetOption->gamma() * quantity; });
        const double vega = safeGreek([&]() { return oneAssetOption->vega() * quantity; });
        const double theta = safeGreek([&]() { return oneAssetOption->theta() * quantity; });
        const double rho = safeGreek([&]() { return oneAssetOption->rho() * quantity; });

        double impliedVol = std::numeric_limits<double>::quiet_NaN();
        if (!opt.hasBarrier) {
            if (auto vanilla = std::dynamic_pointer_cast<QuantLib::VanillaOption>(instrument)) {
                // Without an explicit market price in the request, this is a self-consistency diagnostic:
                // impliedVol(instrument NPV) should return the surface vol used by process (up to tolerance).
                impliedVol = safeGreek([&]() { return vanilla->impliedVolatility(npv, process); });
            }
        }

        auto tradeId = builder->CreateString(opt.tradeId);
        EquityOptionResponseBuilder rb(*builder);
        rb.add_trade_id(tradeId);
        rb.add_npv(scaledNpv);
        rb.add_delta(delta);
        rb.add_gamma(gamma);
        rb.add_vega(vega);
        rb.add_theta(theta);
        rb.add_rho(rho);
        rb.add_implied_volatility(impliedVol);
        rb.add_used_spot(usedSpot);
        rb.add_used_strike(usedStrike);
        rb.add_used_settlement(opt.settlement);
        out.push_back(rb.Finish());
    }

    auto vec = builder->CreateVector(out);
    PriceEquityOptionResponseBuilder rb(*builder);
    rb.add_options(vec);
    return rb.Finish();
}
