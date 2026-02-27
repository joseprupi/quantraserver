#ifndef QUANTRA_SWPTION_PRICING_SERVICE_H
#define QUANTRA_SWPTION_PRICING_SERVICE_H

#include <memory>
#include <string>
#include <unordered_map>

#include <ql/handle.hpp>
#include <ql/pricingengine.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "engine_factory.h"
#include "price_swaption_request_generated.h"
#include "pricing_registry.h"
#include "swaption_model_calibration.h"
#include "swaption_vol_runtime.h"

namespace quantra {

class SwaptionPricingService {
public:
    SwaptionVolEntry resolveVolEntry(
        const SwaptionVolEntry& baseVolEntry,
        const quantra::PriceSwaption* trade,
        const PricingRegistry& reg,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
        bool forceAtmRecompute) const;

    HwCalibResult calibrateHullWhite(
        const PricingRegistry& reg,
        const quantra::SwaptionHwCalibrationSpec* spec,
        const QuantLib::Date& asOf,
        std::unordered_map<std::string, HwCalibResult>& cache,
        const std::string& cacheKey) const;

    std::shared_ptr<QuantLib::PricingEngine> makeEngine(
        EngineFactory& engineFactory,
        const quantra::ModelSpec* model,
        const quantra::SwaptionModelSpec* swaptionModelSpec,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
        const SwaptionVolEntry& volEntry,
        const HwCalibResult* calibrated) const;
};

} // namespace quantra

#endif // QUANTRA_SWPTION_PRICING_SERVICE_H
