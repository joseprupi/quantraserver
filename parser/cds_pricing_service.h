#ifndef QUANTRA_CDS_PRICING_SERVICE_H
#define QUANTRA_CDS_PRICING_SERVICE_H

#include "price_cds_request_generated.h"
#include "pricing_registry.h"

namespace quantra {

struct CdsPriceResult {
    double npv = 0.0;
    double fairSpread = 0.0;
    double fairUpfront = 0.0;
    double defaultLegNpv = 0.0;
    double premiumLegNpv = 0.0;
};

class CdsPricingService {
public:
    CdsPriceResult price(const quantra::PriceCDS* tradePricing, const PricingRegistry& reg) const;
};

} // namespace quantra

#endif // QUANTRA_CDS_PRICING_SERVICE_H
