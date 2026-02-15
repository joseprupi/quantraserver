#include "swaption_pricing_request.h"

#include "pricing_registry.h"
#include "vol_surface_parsers.h"
#include "engine_factory.h"
#include "swaption_parser.h"
#include "curve_bootstrapper.h"
#include "index_registry_builder.h"
#include "swaption_vol_runtime.h"

#include <ql/settings.hpp>
#include <ql/termstructures/volatility/swaption/swaptionconstantvol.hpp>
#include <sstream>
#include <cmath>
#include <ql/utilities/dataformatters.hpp>

#include <ql/pricingengines/blackcalculator.hpp>
#include <ql/pricingengines/bacheliercalculator.hpp>
#include <ql/pricingengines/swap/discountingswapengine.hpp>
#include <ql/instruments/overnightindexedswap.hpp>
#include <ql/instruments/vanillaswap.hpp>
#include <limits>

using namespace QuantLib;
using namespace quantra;

namespace {

struct EvalDateGuard {
    QuantLib::Date saved;
    EvalDateGuard() : saved(QuantLib::Settings::instance().evaluationDate()) {}
    ~EvalDateGuard() { QuantLib::Settings::instance().evaluationDate() = saved; }
};

} // namespace

flatbuffers::Offset<PriceSwaptionResponse> SwaptionPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const PriceSwaptionRequest *request) const
{
    PricingRegistryBuilder regBuilder;
    PricingRegistry reg = regBuilder.build(request->pricing());
    const Date asOf = DateToQL(request->pricing()->as_of_date()->str());
    Settings::instance().evaluationDate() = asOf;

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
        if (vIt->second.referenceDate == QuantLib::Date()) {
            QUANTRA_ERROR("Swaption vol has invalid referenceDate: " + it->volatility()->str());
        }
        if (vIt->second.referenceDate != asOf) {
            std::ostringstream err;
            err << "Strict mode: pricing.as_of_date (" << QuantLib::io::iso_date(asOf)
                << ") must equal swaption vol referenceDate ("
                << QuantLib::io::iso_date(vIt->second.referenceDate)
                << ") for vol '" << it->volatility()->str() << "'";
            QUANTRA_ERROR(err.str());
        }

        auto mIt = reg.models.find(it->model()->str());
        if (mIt == reg.models.end())
            QUANTRA_ERROR("Model not found: " + it->model()->str());

        swaption_parser.linkForwardingTermStructure(fIt->second->currentLink());
        auto swaption = swaption_parser.parse(it->swaption(), reg.indices);

        SwaptionVolEntry volEntry = finalizeSwaptionVolEntryForPricing(
            vIt->second,
            *it,
            reg,
            Handle<YieldTermStructure>(dIt->second->currentLink()),
            Handle<YieldTermStructure>(fIt->second->currentLink()),
            false);

        Handle<YieldTermStructure> discountCurve(dIt->second->currentLink());
        auto engine = engineFactory.makeSwaptionEngine(mIt->second, discountCurve, volEntry);
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

        double impliedVol =
            (volEntry.volKind == quantra::enums::SwaptionVolKind_Constant)
                ? volEntry.constantVol
                : std::numeric_limits<double>::quiet_NaN();
        double atmForward = 0.0;
        double annuity = 0.0;
        double delta = 0.0;
        double vega = 0.0;
        double gamma = 0.0;
        double theta = 0.0;
        double dv01 = 0.0;
        double usedVolatility = 0.0;
        double usedStrike = 0.0;
        double usedAtmForward = -1.0;
        double usedSpreadFromAtm = 0.0;
        double usedCubeNodeAtm = -1.0;
        std::string usedExpiry;
        std::string usedTenor;
        auto volKind = volEntry.volKind;
        auto usedStrikeKind = volEntry.strikeKind;

        auto priceWithRebump = [&](double curveBump, double volBump, int rollDays) {
            EvalDateGuard evalGuard;
            Date baseEval = asOf;
            Date eval = baseEval + rollDays;
            Settings::instance().evaluationDate() = eval;

            // Theta/rebump semantics: we roll evaluationDate and re-bootstrap curves
            // using the same input quotes under the rolled date.
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

            SwaptionVolEntry volEntryBumped = bumpSwaptionVolEntry(volEntry, volBump);
            const bool forceAtmRecompute = (curveBump != 0.0) || (rollDays != 0);
            volEntryBumped = finalizeSwaptionVolEntryForPricing(
                volEntryBumped,
                *it,
                reg,
                Handle<YieldTermStructure>(dItB->second->currentLink()),
                Handle<YieldTermStructure>(fItB->second->currentLink()),
                forceAtmRecompute);

            Handle<YieldTermStructure> discountCurve(dItB->second->currentLink());
            auto bumpEngine = engineFactory.makeSwaptionEngine(mIt->second, discountCurve, volEntryBumped);
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

                if (volEntry.qlVolType == VolatilityType::Normal) {
                    BachelierCalculator calc(optType, strike, atmForward, stdDev, annuity);
                    delta = calc.deltaForward();
                    vega = calc.vega(timeToExpiry);
                    gamma = calc.gammaForward();
                    theta = calc.theta(atmForward, timeToExpiry);
                } else {
                    double displacement = volEntry.displacement;
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
        if (!std::isfinite(impliedVol)) {
            impliedVol = -1.0;
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
        }

        // Debug: compute used vol for this trade
        try {
            if (volEntry.handle.empty()) {
                QUANTRA_ERROR("Swaption vol handle is empty (ATM not injected?)");
            }
            Date evalDate = Settings::instance().evaluationDate();
            Date exerciseDate = swaption->exercise()->dates()[0];
            const auto& volDc = volEntry.dayCounter;
            double optionTime = volDc.yearFraction(evalDate, exerciseDate);
            Date startDate = swaption->underlying()->startDate();
            Date endDate = swaption->underlying()->maturityDate();
            double swapLength = volDc.yearFraction(startDate, endDate);

            usedStrike = getResultOrDefault("strike", 0.0);
            if (usedStrike == 0.0) {
                if (auto vanilla = QuantLib::ext::dynamic_pointer_cast<QuantLib::VanillaSwap>(swaption->underlying())) {
                    usedStrike = vanilla->fixedRate();
                } else if (auto ois = QuantLib::ext::dynamic_pointer_cast<QuantLib::OvernightIndexedSwap>(swaption->underlying())) {
                    usedStrike = ois->fixedRate();
                }
            }

            usedVolatility = volEntry.handle->volatility(optionTime, swapLength, usedStrike);
            const bool hasRealAtm = !volEntry.atmForwardsFlat.empty();
            try {
                auto smile = volEntry.handle->smileSection(optionTime, swapLength);
                if (smile && hasRealAtm) {
                    usedAtmForward = smile->atmLevel();
                    usedCubeNodeAtm = usedAtmForward;
                }
            } catch (...) {
                usedAtmForward = -1.0;
                usedCubeNodeAtm = -1.0;
            }
            if (volEntry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM &&
                usedCubeNodeAtm >= 0.0) {
                usedSpreadFromAtm = usedStrike - usedCubeNodeAtm;
            }

            std::ostringstream expOs;
            expOs << QuantLib::io::iso_date(exerciseDate);
            usedExpiry = expOs.str();

            std::ostringstream tenOs;
            tenOs << QuantLib::io::iso_date(startDate) << "->" << QuantLib::io::iso_date(endDate);
            usedTenor = tenOs.str();
        } catch (...) {
            // best-effort debug, ignore failures
        }

        auto usedExpiryOffset = builder->CreateString(usedExpiry);
        auto usedTenorOffset = builder->CreateString(usedTenor);

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
        response_builder.add_used_volatility(usedVolatility);
        response_builder.add_used_option_expiry(usedExpiryOffset);
        response_builder.add_used_swap_tenor(usedTenorOffset);
        response_builder.add_used_strike(usedStrike);
        response_builder.add_used_atm_forward(usedAtmForward);
        response_builder.add_used_strike_kind(usedStrikeKind);
        response_builder.add_used_spread_from_atm(usedSpreadFromAtm);
        response_builder.add_used_cube_node_atm(usedCubeNodeAtm);
        response_builder.add_vol_kind(volKind);

        swaptions_vector.push_back(response_builder.Finish());
    }

    auto swaptions = builder->CreateVector(swaptions_vector);
    PriceSwaptionResponseBuilder response_builder(*builder);
    response_builder.add_swaptions(swaptions);

    return response_builder.Finish();
}
