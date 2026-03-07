#ifndef QUANTRA_PRICING_REGISTRY_H
#define QUANTRA_PRICING_REGISTRY_H

#include <map>
#include <string>
#include <memory>
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
    IndexRegistry indices;
    SwapIndexRegistry swapIndices;
    std::vector<const quantra::CouponPricer*> couponPricers;
};

struct CreditRegistry {
    std::map<std::string, const quantra::CreditCurveSpec*> creditCurveSpecs;
};

struct VolatilityRegistry {
    std::map<std::string, OptionletVolEntry> optionletVols;
    std::map<std::string, SwaptionVolEntry> swaptionVols;
    std::map<std::string, BlackVolEntry> blackVols;
    std::map<std::string, const quantra::ModelSpec*> models;
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
