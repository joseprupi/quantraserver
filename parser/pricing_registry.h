#ifndef QUANTRA_PRICING_REGISTRY_H
#define QUANTRA_PRICING_REGISTRY_H

/**
 * Pricing Registry
 * 
 * Central registry for all parsed market data in a pricing request.
 * Follows the "curves pattern" - parse once, store by ID, lookup as needed.
 * 
 * This replaces the old VolatilityParser approach with a unified registry
 * that handles curves, quotes, vol surfaces, and models consistently.
 * 
 * CHANGE: Curves are now bootstrapped via CurveBootstrapper, which handles
 * dependency ordering for multi-curve setups (OIS discount + projection).
 * 
 * Usage:
 *   PricingRegistryBuilder builder;
 *   PricingRegistry reg = builder.build(pricing);
 *   
 *   // Lookup curve by ID
 *   auto& curveHandle = reg.curves.at("USD_OIS");
 *   
 *   // Lookup vol by ID (type-specific maps)
 *   auto& volEntry = reg.optionletVols.at("USD_CAP_VOL");
 *   
 *   // Build engine using model + vol
 *   auto engine = factory.makeCapFloorEngine(model, curve, volEntry);
 */

#include <map>
#include <string>
#include <memory>
#include <vector>

#include <ql/handle.hpp>
#include <ql/quote.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "vol_surface_parsers.h"
#include "common_generated.h"
#include "model_generated.h"

namespace quantra {

/**
 * Registry containing all parsed market data.
 * 
 * Design notes:
 *   - curves: RelinkableHandle allows curve relinking without rebuilding trades
 *   - quotes: For equity spots, FX rates, etc.
 *   - optionletVols/swaptionVols/blackVols: Separate maps because QuantLib types
 *     are not interchangeable (prevents accidental misuse)
 *   - models: Stored as pointers to FlatBuffers specs for lazy engine creation
 */
struct PricingRegistry {
    // Yield curves (bootstrapped or flat)
    std::map<std::string, std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>> curves;
    
    // Market quotes (equity spots, FX rates)
    std::map<std::string, QuantLib::Handle<QuantLib::Quote>> quotes;
    
    // Vol surfaces - separate maps by QuantLib type
    // This enforces type safety: can't accidentally use optionlet vol for swaption
    std::map<std::string, OptionletVolEntry> optionletVols;
    std::map<std::string, SwaptionVolEntry> swaptionVols;
    std::map<std::string, BlackVolEntry> blackVols;
    
    // Model specs (lazy - engine created on demand)
    std::map<std::string, const quantra::ModelSpec*> models;

    // Coupon pricers (for floating rate instruments)
    std::vector<const quantra::CouponPricer*> couponPricers;

    // Bond pricing flags
    bool bondPricingDetails = false;
    bool bondPricingFlows = false;
};

/**
 * Builder for PricingRegistry.
 * 
 * Parses the Pricing message and populates all registry maps.
 * Sets the QuantLib evaluation date from as_of_date.
 */
class PricingRegistryBuilder {
public:
    /**
     * Build a PricingRegistry from a Pricing message.
     * 
     * @param pricing The FlatBuffers Pricing object
     * @return Populated PricingRegistry
     * @throws std::runtime_error if validation fails
     */
    PricingRegistry build(const quantra::Pricing* pricing) const;
};

} // namespace quantra

#endif // QUANTRA_PRICING_REGISTRY_H
