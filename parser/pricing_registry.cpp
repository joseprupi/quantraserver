/**
 * Pricing Registry Builder Implementation
 * 
 * Builds the PricingRegistry by delegating to type-specific parsers.
 * 
 * CHANGE: Curves are now bootstrapped via CurveBootstrapper, which handles
 * dependency ordering (e.g., OIS curve before Euribor curve that uses it
 * as exogenous discount).
 */

#include "pricing_registry.h"

#include <ql/settings.hpp>

#include "curve_bootstrapper.h"
#include "enums.h"
#include "common.h"

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

    // Set evaluation date (affects all QuantLib calculations)
    QuantLib::Date asOf = DateToQL(pricing->as_of_date()->str());
    QuantLib::Settings::instance().evaluationDate() = asOf;

    PricingRegistry reg;

    // ==========================================================================
    // Parse Quotes (optional, for equity/FX and now helper quote_id resolution)
    // ==========================================================================
    if (pricing->quotes()) {
        for (auto it = pricing->quotes()->begin(); it != pricing->quotes()->end(); ++it) {
            if (!it->id()) {
                QUANTRA_ERROR("QuoteSpec.id is required");
            }
            std::string id = it->id()->str();
            auto sq = std::make_shared<QuantLib::SimpleQuote>(it->value());
            reg.quotes.emplace(id, QuantLib::Handle<QuantLib::Quote>(sq));
        }
    }

    // ==========================================================================
    // Parse Curves (dependency-aware via CurveBootstrapper)
    //
    // This replaces the old loop that bootstrapped each curve independently.
    // CurveBootstrapper:
    //   1. Creates empty RelinkableHandles for all curves
    //   2. Scans helper deps to build a dependency graph
    //   3. Topologically sorts curves
    //   4. Bootstraps in order, linking handles progressively
    // ==========================================================================
    if (!pricing->curves()) {
        QUANTRA_ERROR("curves is required (at least one curve needed)");
    }

    CurveBootstrapper bootstrapper;
    auto booted = bootstrapper.bootstrapAll(pricing->curves(), pricing->quotes());

    for (auto& kv : booted.handles) {
        reg.curves.emplace(kv.first, kv.second);
    }

    // ==========================================================================
    // Parse Vol Surfaces (optional)
    // Dispatches to type-specific parsers based on payload type
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
                    reg.optionletVols.emplace(id, parseOptionletVol(spec));
                    break;
                    
                case quantra::VolPayload_SwaptionVolSpec:
                    reg.swaptionVols.emplace(id, parseSwaptionVol(spec));
                    break;
                    
                case quantra::VolPayload_BlackVolSpec:
                    reg.blackVols.emplace(id, parseBlackVol(spec));
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
    // Models are stored as pointers - engine creation is deferred to request time
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
    // Coupon pricers (optional)
    // ==========================================================================
    if (pricing->coupon_pricers()) {
        for (auto it = pricing->coupon_pricers()->begin(); it != pricing->coupon_pricers()->end(); ++it) {
            reg.couponPricers.emplace_back(*it);
        }
    }

    reg.bondPricingDetails = pricing->bond_pricing_details();
    reg.bondPricingFlows = pricing->bond_pricing_flows();

    return reg;
}

} // namespace quantra
