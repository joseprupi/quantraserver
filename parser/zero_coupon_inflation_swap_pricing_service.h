#ifndef QUANTRA_ZERO_COUPON_INFLATION_SWAP_PRICING_SERVICE_H
#define QUANTRA_ZERO_COUPON_INFLATION_SWAP_PRICING_SERVICE_H

#include <ql/cashflow.hpp>

#include "price_zero_coupon_inflation_swap_request_generated.h"
#include "pricing_registry.h"

namespace quantra {

struct ZeroCouponInflationSwapPriceResult {
    double npv = 0.0;
    double fairRate = 0.0;
    double fixedLegBps = 0.0;
    double fixedLegNpv = 0.0;
    double inflationLegNpv = 0.0;
    QuantLib::Leg fixedLeg;
    QuantLib::Leg inflationLeg;
};

class ZeroCouponInflationSwapPricingService {
public:
    ZeroCouponInflationSwapPriceResult price(
        const quantra::PriceZeroCouponInflationSwap* tradePricing,
        const PricingRegistry& reg) const;
};

} // namespace quantra

#endif // QUANTRA_ZERO_COUPON_INFLATION_SWAP_PRICING_SERVICE_H
