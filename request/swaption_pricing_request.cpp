#include "swaption_pricing_request.h"

#include "pricing_registry.h"
#include "vol_surface_parsers.h"
#include "engine_factory.h"
#include "swaption_parser.h"
#include "curve_bootstrapper.h"
#include "index_registry_builder.h"

#include <ql/settings.hpp>
#include <ql/termstructures/volatility/swaption/swaptionconstantvol.hpp>

#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/pricingengines/bacheliercalculator.hpp>

using namespace QuantLib;
using namespace quantra;

flatbuffers::Offset<PriceSwaptionResponse> SwaptionPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceSwaptionRequest *request) const
{
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());

    SwaptionParser swaption_parser;
    EngineFactory engineFactory;

    auto swaption_pricings = request->swaptions();
    std::vector<flatbuffers::Offset<SwaptionResponse>> swaptions_vector;

    for (auto it = swaption_pricings->begin(); it != swaption_pricings->end(); it++)
    {
        auto dIt = reg.curves.find(it->discounting_curve()->str());
        if (dIt == reg.curves.end())
            QUANTRA_ERROR("Discounting curve not found: " + it->discounting_curve()->str());

        auto fIt = reg.curves.find(it->forwarding_curve()->str());
        if (fIt == reg.curves.end())
            QUANTRA_ERROR("Forwarding curve not found: " + it->forwarding_curve()->str());

        auto vIt = reg.swaptionVols.find(it->volatility()->str());
        if (vIt == reg.swaptionVols.end())
            QUANTRA_ERROR("Swaption vol not found: " + it->volatility()->str());

        auto mIt = reg.models.find(it->model()->str());
        if (mIt == reg.models.end())
            QUANTRA_ERROR("Model not found: " + it->model()->str());

        swaption_parser.linkForwardingTermStructure(fIt->second->currentLink());
        auto swaption = swaption_parser.parse(it->swaption(), reg.indices);

        Handle<YieldTermStructure> discountCurve(dIt->second->currentLink());
        auto engine = engineFactory.makeSwaptionEngine(mIt->second, discountCurve, vIt->second);
        swaption->setPricingEngine(engine);

        double npv = swaption->NPV();

        std::cout << "Swaption NPV: " << npv << std::endl;

        auto getResultOrDefault = [&](const std::string& key, double fallback) {
            try {
                return swaption->result<double>(key);
            } catch (...) {
                return fallback;
            }
        };

        double impliedVol = vIt->second.constantVol;
        double atmForward = 0.0;
        double annuity = 0.0;
        double delta = 0.0;
        double vega = 0.0;
        double gamma = 0.0;
        double theta = 0.0;
        double dv01 = 0.0;

        auto priceWithRebump = [&](double curveBump, double volBump, int rollDays) {
            Date baseEval = DateToQL(request->pricing()->as_of_date()->str());
            Date eval = baseEval + rollDays;
            Settings::instance().evaluationDate() = eval;

            CurveBootstrapper bootstrapper;
            auto booted = bootstrapper.bootstrapAll(
                request->pricing()->curves(),
                request->pricing()->quotes(),
                request->pricing()->indices(),
                curveBump
            );

            auto dItB = booted.handles.find(it->discounting_curve()->str());
            if (dItB == booted.handles.end())
                QUANTRA_ERROR("Discounting curve not found (rebump): " + it->discounting_curve()->str());

            auto fItB = booted.handles.find(it->forwarding_curve()->str());
            if (fItB == booted.handles.end())
                QUANTRA_ERROR("Forwarding curve not found (rebump): " + it->forwarding_curve()->str());

            IndexRegistryBuilder indexBuilder;
            IndexRegistry idx = indexBuilder.build(request->pricing()->indices());

            SwaptionParser bumpParser;
            bumpParser.linkForwardingTermStructure(fItB->second->currentLink());
            auto bumpSwaption = bumpParser.parse(it->swaption(), idx);

            SwaptionVolEntry volEntry = vIt->second;
            if (volBump != 0.0) {
                double bumpedVol = vIt->second.constantVol + volBump;
                if (bumpedVol <= 0.0) bumpedVol = 1.0e-8;
                volEntry.constantVol = bumpedVol;
                auto volTerm = std::make_shared<ConstantSwaptionVolatility>(
                    vIt->second.referenceDate,
                    vIt->second.calendar,
                    Following,
                    bumpedVol,
                    vIt->second.dayCounter,
                    vIt->second.qlVolType,
                    vIt->second.displacement);
                volEntry.handle = Handle<SwaptionVolatilityStructure>(volTerm);
            }

            Handle<YieldTermStructure> discountCurve(dItB->second->currentLink());
            auto bumpEngine = engineFactory.makeSwaptionEngine(mIt->second, discountCurve, volEntry);
            bumpSwaption->setPricingEngine(bumpEngine);
            return bumpSwaption->NPV();
        };

        if (reg.swaptionPricingDetails) {
            impliedVol = getResultOrDefault("impliedVolatility", impliedVol);
            atmForward = getResultOrDefault("atmForward", 0.0);
            annuity = getResultOrDefault("annuity", 0.0);

            double strike = getResultOrDefault("strike", 0.0);
            double stdDev = getResultOrDefault("stdDev", 0.0);
            double timeToExpiry = getResultOrDefault("timeToExpiry", 0.0);

            if (annuity != 0.0 && stdDev > 0.0 && timeToExpiry > 0.0) {
                Option::Type optType =
                    (swaption->underlying()->type() == Swap::Payer) ? Option::Call : Option::Put;

                if (vIt->second.qlVolType == VolatilityType::Normal) {
                    BachelierCalculator calc(optType, strike, atmForward, stdDev, annuity);
                    delta = calc.deltaForward();
                    vega = calc.vega(timeToExpiry);
                    gamma = calc.gammaForward();
                    theta = calc.theta(atmForward, timeToExpiry);
                } else {
                    double displacement = vIt->second.displacement;
                    BlackCalculator calc(optType, strike + displacement, atmForward + displacement, stdDev, annuity);
                    delta = calc.deltaForward();
                    vega = calc.vega(timeToExpiry);
                    gamma = calc.gammaForward();
                    theta = calc.theta(atmForward + displacement, timeToExpiry);
                }
            }

            // DV01 as price change for a 1bp move in the swap rate
            dv01 = delta * 1.0e-4;
        }

        if (reg.swaptionPricingRebump) {
            const double bump = 1.0e-4; // 1bp
            double npvUp = priceWithRebump(bump, 0.0, 0);
            double npvDown = priceWithRebump(-bump, 0.0, 0);
            dv01 = (npvUp - npvDown) / 2.0;
            gamma = (npvUp - 2.0 * npv + npvDown);

            double volUp = priceWithRebump(0.0, bump, 0);
            double volDown = priceWithRebump(0.0, -bump, 0);
            vega = (volUp - volDown) / 2.0;

            double npvTomorrow = priceWithRebump(0.0, 0.0, 1);
            theta = npvTomorrow - npv;

            // Reset evaluation date to base
            Settings::instance().evaluationDate() = DateToQL(request->pricing()->as_of_date()->str());
        }

        SwaptionResponseBuilder response_builder(*builder);
        response_builder.add_npv(npv);
        response_builder.add_implied_volatility(impliedVol);
        response_builder.add_atm_forward(atmForward);
        response_builder.add_annuity(annuity);
        response_builder.add_delta(delta);
        response_builder.add_vega(vega);
        response_builder.add_gamma(gamma);
        response_builder.add_theta(theta);
        response_builder.add_dv01(dv01);

        swaptions_vector.push_back(response_builder.Finish());
    }

    auto swaptions = builder->CreateVector(swaptions_vector);
    PriceSwaptionResponseBuilder response_builder(*builder);
    response_builder.add_swaptions(swaptions);

    return response_builder.Finish();
}
