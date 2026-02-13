/**
 * Pricing Registry Builder Implementation
 * 
 * Builds the PricingRegistry by delegating to type-specific parsers.
 * Now includes IndexRegistry built from pricing->indices().
 */

#include "pricing_registry.h"

#include <ql/settings.hpp>

#include "curve_bootstrapper.h"
#include "index_registry_builder.h"
#include "enums.h"
#include "common.h"
#include "quote_registry.h"

namespace quantra {

PricingRegistry PricingRegistryBuilder::build(const quantra::Pricing* pricing) const {
    // ==========================================================================
    // Validation
    // ==========================================================================
    if (!pricing) {
        QUANTRA_ERROR("Pricing not found");
    }
    if (!pricing->as_of_date()) {
        QUANTRA_ERROR("as_of_date is required");
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
                QUANTRA_ERROR("QuoteSpec.id is required");
            }
            std::string id = it->id()->str();
            auto sq = std::make_shared<QuantLib::SimpleQuote>(it->value());
            quoteRegistry.upsert(id, it->value(), it->quote_type());
        }
    }
    reg.quoteRegistry = quoteRegistry;

    // ==========================================================================
    // Build IndexRegistry from indices[]
    // ==========================================================================
    IndexRegistryBuilder indexBuilder;
    reg.indices = indexBuilder.build(pricing->indices());

    // ==========================================================================
    // Parse Curves (dependency-aware via CurveBootstrapper)
    // Now passes indices to CurveBootstrapper for helper index resolution
    // ==========================================================================
    if (!pricing->curves()) {
        QUANTRA_ERROR("curves is required (at least one curve needed)");
    }

    CurveBootstrapper bootstrapper;
    auto booted = bootstrapper.bootstrapAll(
        pricing->curves(),
        pricing->quotes(),
        pricing->indices()
    );

    for (auto& kv : booted.handles) {
        reg.curves.emplace(kv.first, kv.second);
    }

    // ==========================================================================
    // Parse Vol Surfaces (optional)
    // ==========================================================================
    if (pricing->vol_surfaces()) {
        for (auto it = pricing->vol_surfaces()->begin(); it != pricing->vol_surfaces()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_ERROR("VolSurfaceSpec.id is required");
            }
            std::string id = spec->id()->str();

            switch (spec->payload_type()) {
                case quantra::VolPayload_OptionletVolSpec:
                    reg.optionletVols.emplace(id, parseOptionletVol(spec, &quoteRegistry));
                    break;
                    
                case quantra::VolPayload_SwaptionVolSpec:
                    reg.swaptionVols.emplace(id, parseSwaptionVol(spec, &quoteRegistry));
                    break;
                    
                case quantra::VolPayload_BlackVolSpec:
                    reg.blackVols.emplace(id, parseBlackVol(spec, &quoteRegistry));
                    break;
                    
                case quantra::VolPayload_NONE:
                    QUANTRA_ERROR("VolSurfaceSpec.payload is required for vol id: " + id);
                    
                default:
                    QUANTRA_ERROR("Unknown VolPayload type for vol id: " + id);
            }
        }
    }

    // ==========================================================================
    // Parse Models (optional)
    // ==========================================================================
    if (pricing->models()) {
        for (auto it = pricing->models()->begin(); it != pricing->models()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_ERROR("ModelSpec.id is required");
            }
            std::string id = spec->id()->str();
            
            if (spec->payload_type() == quantra::ModelPayload_NONE) {
                QUANTRA_ERROR("ModelSpec.payload is required for model id: " + id);
            }
            
            reg.models[id] = spec;
        }
    }

    // ==========================================================================
    // Register Credit Curve Specs (optional)
    // ==========================================================================
    if (pricing->credit_curves()) {
        for (auto it = pricing->credit_curves()->begin(); it != pricing->credit_curves()->end(); ++it) {
            const auto* spec = *it;
            if (!spec->id()) {
                QUANTRA_ERROR("CreditCurveSpec.id is required");
            }
            if (!spec->reference_date()) {
                QUANTRA_ERROR("CreditCurveSpec.reference_date is required");
            }
            reg.creditCurveSpecs.emplace(spec->id()->str(), spec);
        }
    }

    // ==========================================================================
    // Coupon pricers (optional)
    // ==========================================================================
    if (pricing->coupon_pricers()) {
        for (auto it = pricing->coupon_pricers()->begin(); it != pricing->coupon_pricers()->end(); ++it) {
            reg.couponPricers.emplace_back(*it);
        }
    }

    reg.bondPricingDetails = pricing->bond_pricing_details();
    reg.bondPricingFlows = pricing->bond_pricing_flows();
    reg.swaptionPricingDetails = pricing->swaption_pricing_details();
    reg.swaptionPricingRebump = pricing->swaption_pricing_rebump();

    return reg;
}

} // namespace quantra
