#include "swaption_pricing_service.h"

namespace quantra {

SwaptionVolEntry SwaptionPricingService::resolveVolEntry(
    const SwaptionVolEntry& baseVolEntry,
    const quantra::PriceSwaption* trade,
    const PricingRegistry& reg,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve,
    bool forceAtmRecompute) const {
    return finalizeSwaptionVolEntryForPricing(
        baseVolEntry,
        trade,
        reg,
        discountCurve,
        forwardingCurve,
        forceAtmRecompute);
}

HwCalibResult SwaptionPricingService::calibrateHullWhite(
    const PricingRegistry& reg,
    const quantra::SwaptionHwCalibrationSpec* spec,
    const QuantLib::Date& asOf,
    std::unordered_map<std::string, HwCalibResult>& cache,
    const std::string& cacheKey) const {
    auto it = cache.find(cacheKey);
    if (it != cache.end()) {
        return it->second;
    }
    HwCalibResult res = calibrateHullWhiteFromSwaptionVol(reg, spec, asOf);
    cache.emplace(cacheKey, res);
    return res;
}

std::shared_ptr<QuantLib::PricingEngine> SwaptionPricingService::makeEngine(
    EngineFactory& engineFactory,
    const quantra::ModelSpec* model,
    const quantra::SwaptionModelSpec* swaptionModelSpec,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const SwaptionVolEntry& volEntry,
    const HwCalibResult* calibrated) const {
    if (swaptionModelSpec->model_type() == quantra::enums::IrModelType_HullWhiteLattice &&
        swaptionModelSpec->param_mode() == quantra::enums::ModelParamMode_Calibrate &&
        calibrated != nullptr) {
        return engineFactory.makeHullWhiteLatticeSwaptionEngine(
            discountCurve,
            calibrated->a,
            calibrated->sigma,
            swaptionModelSpec->lattice_steps());
    }
    return engineFactory.makeSwaptionEngine(model, discountCurve, volEntry);
}

} // namespace quantra
