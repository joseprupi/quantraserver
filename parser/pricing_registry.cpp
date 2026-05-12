/**
 * Pricing Registry Builder Implementation
 *
 * Builds the PricingRegistry by delegating to domain-specific parsers.
 */

#include "pricing_registry.h"

#include <ql/settings.hpp>

#include "curve_bootstrapper.h"
#include "index_registry_builder.h"
#include "swap_index_registry.h"
#include "enums.h"
#include "common.h"
#include "quote_registry.h"
#include "inflation_curve_parsers.h"

namespace quantra {

PricingRegistry PricingRegistryBuilder::build(const quantra::Pricing* pricing) const {
    // ==========================================================================
    // Validation
    // ==========================================================================
    if (!pricing) {
        QUANTRA_INVALID_ARGUMENT("Pricing not found");
    }
    if (!pricing->as_of_date()) {
        QUANTRA_INVALID_ARGUMENT("as_of_date is required");
    }

    // Set evaluation date
    QuantLib::Date asOf = DateToQL(pricing->as_of_date()->str());
    QuantLib::Settings::instance().evaluationDate() = asOf;

    PricingRegistry reg;

    // ==========================================================================
    // Parse Quotes (optional)
    // ==========================================================================
    QuoteRegistry quoteRegistry;
    if (pricing->quotes()) {
        for (auto it = pricing->quotes()->begin(); it != pricing->quotes()->end(); ++it) {
            if (!it->id()) {
                QUANTRA_INVALID_ARGUMENT("QuoteSpec.id is required");
            }
            std::string id = it->id()->str();
            auto sq = std::make_shared<QuantLib::SimpleQuote>(it->value());
            quoteRegistry.upsert(id, it->value(), it->quote_type());
        }
    }
    reg.quoteRegistry = quoteRegistry;

    const auto* rates = pricing->rates();
    const auto* credit = pricing->credit();
    const auto* volatility = pricing->volatility();
    const auto* inflation = pricing->inflation();
    const auto* options = pricing->options();

    // ==========================================================================
    // Build rates domain
    // ==========================================================================
    IndexRegistryBuilder indexBuilder;
    reg.rates.indices = indexBuilder.build(rates ? rates->indices() : nullptr);
    SwapIndexRegistryBuilder swapIndexBuilder;
    reg.rates.swapIndices = swapIndexBuilder.build(
        rates ? rates->swap_indices() : nullptr,
        reg.rates.indices);

    // ==========================================================================
    // Parse Curves (dependency-aware via CurveBootstrapper)
    // ==========================================================================
    if (!rates || !rates->curves()) {
        QUANTRA_INVALID_ARGUMENT("pricing.rates.curves is required (at least one curve needed)");
    }

    CurveBootstrapper bootstrapper;
    auto booted = bootstrapper.bootstrapAll(
        rates->curves(),
        pricing->quotes(),
        rates ? rates->indices() : nullptr
    );

    reg.rates.curveKeys = booted.keys;
    for (auto& kv : booted.handles) {
        reg.rates.curves.emplace(kv.first, kv.second);
    }

    // ==========================================================================
    // Parse inflation domain (optional)
    // ==========================================================================
    if (inflation && inflation->inflation_curves()) {
        reg.inflation.curveMetadata = buildInflationCurves(
            inflation->inflation_curves(),
            inflation->inflation_indices(),
            &quoteRegistry,
            reg);
        buildInflationIndices(inflation->inflation_indices(), reg.inflation.curveMetadata, reg);
    } else if (inflation && inflation->inflation_indices()) {
        buildInflationIndices(inflation->inflation_indices(), reg.inflation.curveMetadata, reg);
    }

    // ==========================================================================
    // Parse volatility domain (optional)
    // Parsed after rates so equity SurfaceFromPrices can resolve curve ids.
    // ==========================================================================
    if (volatility && volatility->vol_surfaces()) {
        for (auto it = volatility->vol_surfaces()->begin(); it != volatility->vol_surfaces()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec.id is required");
            }
            std::string id = spec->id()->str();

            switch (spec->payload_type()) {
                case quantra::VolPayload_OptionletVolSpec:
                    reg.volatility.optionletVols.emplace(id, parseOptionletVol(spec, &quoteRegistry));
                    break;

                case quantra::VolPayload_SwaptionVolSpec:
                    reg.volatility.swaptionVols.emplace(id, parseSwaptionVol(spec, &quoteRegistry));
                    break;

                case quantra::VolPayload_BlackVolSpec:
                    reg.volatility.blackVols.emplace(id, parseBlackVol(spec, &quoteRegistry, &reg.rates.curves));
                    break;

                case quantra::VolPayload_NONE:
                    QUANTRA_INVALID_ARGUMENT("VolSurfaceSpec.payload is required for vol id: " + id);

                default:
                    QUANTRA_INVALID_ARGUMENT("Unknown VolPayload type for vol id: " + id);
            }
        }
    }
    for (const auto& kv : reg.volatility.swaptionVols) {
        const auto& entry = kv.second;
        const bool needsSwapIndex =
            entry.strikeKind == quantra::enums::SwaptionStrikeKind_SpreadFromATM ||
            entry.volKind == quantra::enums::SwaptionVolKind_SabrParams ||
            entry.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate;
        if (needsSwapIndex) {
            if (entry.swapIndexId.empty()) {
                QUANTRA_INVALID_ARGUMENT(
                    "Swaption vol '" + kv.first +
                    "' requires swap_index_id (forward-aware surface)");
            }
            if (!reg.rates.swapIndices.has(entry.swapIndexId)) {
                QUANTRA_NOT_FOUND(
                    "Swaption vol '" + kv.first + "' references unknown swap_index_id: " +
                    entry.swapIndexId);
            }
        }
        // SABR calibrate path requires an Ibor swap index because QuantLib's
        // XabrSwaptionVolatilityCube takes a QuantLib::SwapIndex (Ibor-shaped)
        // and we route through SwapIndexRegistry::getIborSwapIndexWithCurves.
        // OIS-shaped indices need a parallel helper (e.g. an OIS-aware
        // construction with OvernightIndexedSwapIndex); deferred to a
        // follow-up.
        if (entry.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate) {
            const auto& sidx = reg.rates.swapIndices.get(entry.swapIndexId);
            if (sidx.kind != quantra::SwapIndexKind_IborSwapIndex) {
                QUANTRA_INVALID_ARGUMENT(
                    "Swaption vol '" + kv.first +
                    "' uses SabrCalibrate with swap_index_id '" + entry.swapIndexId +
                    "' which is not an Ibor swap index; OIS swap index support for the "
                    "SABR calibrate path is not implemented yet (TODO).");
            }
        }
    }

    // ==========================================================================
    // Parse models (optional)
    // ==========================================================================
    if (volatility && volatility->models()) {
        for (auto it = volatility->models()->begin(); it != volatility->models()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_INVALID_ARGUMENT("ModelSpec.id is required");
            }
            std::string id = spec->id()->str();
            
            if (spec->payload_type() == quantra::ModelPayload_NONE) {
                QUANTRA_INVALID_ARGUMENT("ModelSpec.payload is required for model id: " + id);
            }

            reg.volatility.models[id] = spec;
        }
    }

    // ==========================================================================
    // Register credit curve specs (optional)
    // ==========================================================================
    if (credit && credit->credit_curves()) {
        for (auto it = credit->credit_curves()->begin(); it != credit->credit_curves()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_INVALID_ARGUMENT("CreditCurveSpec.id is required");
            }
            if (!spec->reference_date()) {
                QUANTRA_INVALID_ARGUMENT("CreditCurveSpec.reference_date is required");
            }
            reg.credit.creditCurveSpecs.emplace(spec->id()->str(), spec);
        }
    }

    // ==========================================================================
    // Coupon pricers (optional)
    // ==========================================================================
    if (rates && rates->coupon_pricers()) {
        for (auto it = rates->coupon_pricers()->begin(); it != rates->coupon_pricers()->end(); ++it) {
            reg.rates.couponPricers.emplace_back(*it);
        }
    }

    if (options) {
        reg.options.bondPricingDetails = options->bond_pricing_details();
        reg.options.bondPricingFlows = options->bond_pricing_flows();
        reg.options.swaptionPricingDetails = options->swaption_pricing_details();
        reg.options.swaptionPricingRebump = options->swaption_pricing_rebump();
    }

    return reg;
}

} // namespace quantra
