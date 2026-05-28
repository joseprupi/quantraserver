#ifndef QUANTRA_PRICING_REGISTRY_H
#define QUANTRA_PRICING_REGISTRY_H

#include <map>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>

#include <ql/handle.hpp>
#include <ql/quote.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/termstructures/inflationtermstructure.hpp>
#include <ql/indexes/inflationindex.hpp>
#include <ql/time/date.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/businessdayconvention.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/time/period.hpp>

#include "vol_surface_parsers.h"
#include "common_generated.h"
#include "pricing_generated.h"
#include "credit_curve_generated.h"
#include "coupon_pricer_generated.h"
#include "model_generated.h"
#include "credit_curve_domain.h"
#include "equity_underlying_registry.h"
#include "model_domain.h"
#include "coupon_pricer_domain.h"
#include "index_registry.h"
#include "swap_index_registry.h"
#include "quote_registry.h"

namespace QuantLib {
class DefaultProbabilityTermStructure;
}

namespace quantra {

struct InflationCurveEntry {
    enums::InflationCurveKind kind = enums::InflationCurveKind_ZeroInflation;
    std::string id;
    std::string indexId;
    QuantLib::Date referenceDate;
    QuantLib::Calendar calendar;
    QuantLib::BusinessDayConvention businessDayConvention = QuantLib::ModifiedFollowing;
    QuantLib::DayCounter dayCounter;
    QuantLib::Period observationLag;
    QuantLib::Frequency frequency = QuantLib::Monthly;
    bool indexInterpolated = true;
    bool allowExtrapolation = true;
    std::vector<QuantLib::Date> pillarDates;
};

struct RatesRegistry {
    std::map<std::string, std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>> curves;
    /// Per-curve canonical cache keys ("yc:v1:<hex>"), populated when the curve
    /// cache is enabled. Used by downstream consumers (e.g. SABR calibrate
    /// cube cache) to derive their own content-keyed identities. Missing
    /// entries indicate that the curve was bootstrapped with caching disabled
    /// (e.g. under a non-zero curveBump), and downstream caches should fall
    /// back to bypassing their own cache for that surface.
    std::unordered_map<std::string, std::string> curveKeys;
    IndexRegistry indices;
    SwapIndexRegistry swapIndices;
    // Legacy: raw FlatBuffers pointers. Kept until every consumer has been
    // cut over to the plain-domain map below.
    std::vector<const quantra::CouponPricer*> couponPricers;
    // Plain-domain mirror, populated alongside `couponPricers` for every
    // entry on every request. New consumers must read from this map.
    std::map<std::string, CouponPricerDomain> couponPricerDomains;
};

struct CreditRegistry {
    // Legacy: raw FlatBuffers pointer. Kept until every consumer has been
    // cut over to the plain-domain map below.
    std::map<std::string, const quantra::CreditCurveSpec*> creditCurveSpecs;
    // Plain-domain mirror, populated alongside `creditCurveSpecs` for every
    // entry on every request. New consumers must read from this map.
    std::map<std::string, CreditCurveDomain> creditCurves;
};

struct VolatilityRegistry {
    std::map<std::string, OptionletVolEntry> optionletVols;
    std::map<std::string, SwaptionVolEntry> swaptionVols;
    std::map<std::string, BlackVolEntry> blackVols;
    // Legacy: raw FlatBuffers pointer. Kept until every consumer has been
    // cut over to the plain-domain map below.
    std::map<std::string, const quantra::ModelSpec*> models;
    // Plain-domain mirror, populated alongside `models` for every entry on
    // every request. New consumers must read from this map.
    std::map<std::string, ModelDomain> modelDomains;
};

struct EquityRegistry {
    /// Plain-domain registry of equity underlyings keyed by id, populated
    /// from pricing.equity.equity_underlyings via EquityUnderlyingRegistryBuilder.
    std::unordered_map<std::string, EquityUnderlyingRuntime> equityUnderlyings;
};

struct InflationRegistry {
    std::map<std::string, std::shared_ptr<QuantLib::InflationIndex>> inflationIndices;
    std::map<std::string, std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::ZeroInflationTermStructure>>> zeroInflationCurves;
    std::map<std::string, std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YoYInflationTermStructure>>> yoyInflationCurves;
    std::map<std::string, InflationCurveEntry> curveMetadata;
};

struct PricingRequestOptions {
    bool bondPricingDetails = false;
    bool bondPricingFlows = false;
    bool swaptionPricingDetails = false;
    bool swaptionPricingRebump = false;
};

/**
 * Registry containing all parsed market data.
 */
struct PricingRegistry {
    // Shared market quotes (type-checked)
    QuoteRegistry quoteRegistry;

    RatesRegistry rates;
    CreditRegistry credit;
    VolatilityRegistry volatility;
    EquityRegistry equity;
    InflationRegistry inflation;
    PricingRequestOptions options;
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
