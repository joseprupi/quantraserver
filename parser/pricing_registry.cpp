/**
 * Pricing Registry Builder Implementation
 * 
 * Builds the PricingRegistry by delegating to type-specific parsers.
 */

#include "pricing_registry.h"

#include <ql/settings.hpp>

#include "term_structure_parser.h"
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
    // Parse Curves
    // ==========================================================================
    if (!pricing->curves()) {
        QUANTRA_ERROR("curves is required (at least one curve needed)");
    }

    TermStructureParser tsParser;
    for (auto it = pricing->curves()->begin(); it != pricing->curves()->end(); ++it) {
        if (!it->id()) {
            QUANTRA_ERROR("TermStructure.id is required");
        }
        std::string id = it->id()->str();
        
        auto handle = std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>();
        auto curve = tsParser.parse(*it);
        handle->linkTo(curve);
        reg.curves.emplace(id, handle);
    }

    // ==========================================================================
    // Parse Quotes (optional, for equity/FX)
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

    return reg;
}

} // namespace quantra
