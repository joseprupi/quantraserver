#ifndef QUANTRA_VANILLA_SWAP_PRICING_SERVICE_H
#define QUANTRA_VANILLA_SWAP_PRICING_SERVICE_H

#include <ql/cashflow.hpp>

#include "cms_leg_parser.h"
#include "price_vanilla_swap_request_generated.h"
#include "pricing_registry.h"

namespace quantra {

struct VanillaSwapPriceResult {
    double npv = 0.0;
    double fairRate = 0.0;
    double fairSpread = 0.0;
    double fixedLegNpv = 0.0;
    double floatingLegNpv = 0.0;
    double fixedLegBps = 0.0;
    double floatingLegBps = 0.0;
    QuantLib::Leg fixedLeg;
    QuantLib::Leg floatingLeg;
    CmsPricerUsed cmsUsed{};
};

class VanillaSwapPricingService {
public:
    VanillaSwapPriceResult price(
        const quantra::PriceVanillaSwap* tradePricing,
        const PricingRegistry& reg,
        const QuantLib::Date& asOf) const;
};

} // namespace quantra

#endif // QUANTRA_VANILLA_SWAP_PRICING_SERVICE_H
