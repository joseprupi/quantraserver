#ifndef QUANTRA_ENGINE_FACTORY_H
#define QUANTRA_ENGINE_FACTORY_H

/**
 * Engine Factory
 * 
 * Creates QuantLib pricing engines based on model specifications.
 * Validates model/vol compatibility before engine creation.
 * 
 * Validation rules:
 *   - Bachelier model requires Normal vols
 *   - Black model requires displacement = 0 (pure lognormal)
 *   - ShiftedBlack model requires displacement > 0
 * 
 * These validations catch configuration errors early rather than
 * producing silently wrong prices.
 */

#include <memory>

#include <ql/pricingengine.hpp>
#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "model_generated.h"
#include "vol_surface_parsers.h"
#include "common.h"

namespace quantra {

class EngineFactory {
public:
    /**
     * Create a cap/floor pricing engine.
     * 
     * @param model ModelSpec with CapFloorModelSpec payload
     * @param discountCurve Discount curve handle
     * @param volEntry Optionlet vol entry with handle + metadata
     * @return Configured pricing engine
     * @throws std::runtime_error if model/vol incompatible
     */
    std::shared_ptr<QuantLib::PricingEngine> makeCapFloorEngine(
        const quantra::ModelSpec* model,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
        const OptionletVolEntry& volEntry) const;

    /**
     * Create a swaption pricing engine.
     * 
     * @param model ModelSpec with SwaptionModelSpec payload
     * @param discountCurve Discount curve handle
     * @param volEntry Swaption vol entry with handle + metadata
     * @return Configured pricing engine
     * @throws std::runtime_error if model/vol incompatible
     */
    std::shared_ptr<QuantLib::PricingEngine> makeSwaptionEngine(
        const quantra::ModelSpec* model,
        const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
        const SwaptionVolEntry& volEntry) const;
        
    // Future: makeEquityEngine for equity options
    // std::shared_ptr<QuantLib::PricingEngine> makeEquityEngine(
    //     const quantra::ModelSpec* model,
    //     const QuantLib::Handle<QuantLib::Quote>& spot,
    //     const QuantLib::Handle<QuantLib::YieldTermStructure>& riskFreeRate,
    //     const QuantLib::Handle<QuantLib::YieldTermStructure>& dividendYield,
    //     const BlackVolEntry& volEntry) const;
};

} // namespace quantra

#endif // QUANTRA_ENGINE_FACTORY_H
