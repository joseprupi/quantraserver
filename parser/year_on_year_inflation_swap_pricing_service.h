#ifndef QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_PRICING_SERVICE_H
#define QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_PRICING_SERVICE_H

#include <ql/cashflow.hpp>

#include "price_year_on_year_inflation_swap_request_generated.h"
#include "pricing_registry.h"

namespace quantra {

struct YearOnYearInflationSwapPriceResult {
    double npv = 0.0;
    double fairRate = 0.0;
    double fairSpread = 0.0;
    double fixedLegBps = 0.0;
    double yoyLegBps = 0.0;
    double fixedLegNpv = 0.0;
    double yoyLegNpv = 0.0;
    QuantLib::Leg fixedLeg;
    QuantLib::Leg yoyLeg;
};

class YearOnYearInflationSwapPricingService {
public:
    YearOnYearInflationSwapPriceResult price(
        const quantra::PriceYearOnYearInflationSwap* tradePricing,
        const PricingRegistry& reg) const;
};

} // namespace quantra

#endif // QUANTRA_YEAR_ON_YEAR_INFLATION_SWAP_PRICING_SERVICE_H
