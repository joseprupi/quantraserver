#ifndef QUANTRA_PRICING_REGISTRY_H
#define QUANTRA_PRICING_REGISTRY_H

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
#include "index_registry.h"

namespace QuantLib {
class DefaultProbabilityTermStructure;
}

namespace quantra {

/**
 * Registry containing all parsed market data.
 */
struct PricingRegistry {
    // Yield curves (bootstrapped or flat)
    std::map<std::string, std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>> curves;

    // Credit curve specs (parsed on demand per CDS trade)
    std::map<std::string, const quantra::CreditCurveSpec*> creditCurveSpecs;
    
    // Market quotes (equity spots, FX rates)
    std::map<std::string, QuantLib::Handle<QuantLib::Quote>> quotes;

    // Index registry (IborIndex and OvernightIndex objects by id)
    IndexRegistry indices;
    
    // Vol surfaces - separate maps by QuantLib type
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

    // Swaption analytics flag
    bool swaptionPricingDetails = false;
    bool swaptionPricingRebump = false;
};

/**
 * Builder for PricingRegistry.
 */
class PricingRegistryBuilder {
public:
    PricingRegistry build(const quantra::Pricing* pricing) const;
};

} // namespace quantra

#endif // QUANTRA_PRICING_REGISTRY_H
