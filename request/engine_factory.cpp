/**
 * Engine Factory Implementation
 * 
 * Creates pricing engines with model/vol validation.
 */

#include "engine_factory.h"

#include <ql/pricingengines/capfloor/bacheliercapfloorengine.hpp>
#include <ql/pricingengines/capfloor/blackcapfloorengine.hpp>
#include <ql/pricingengines/swaption/blackswaptionengine.hpp>

// Check if BachelierSwaptionEngine exists (QuantLib 1.20+)
#if __has_include(<ql/pricingengines/swaption/bachelierswaptionengine.hpp>)
    #include <ql/pricingengines/swaption/bachelierswaptionengine.hpp>
    #define QL_HAS_BACHELIER_SWAPTION_ENGINE 1
#else
    #define QL_HAS_BACHELIER_SWAPTION_ENGINE 0
#endif

namespace quantra {

std::shared_ptr<QuantLib::PricingEngine> EngineFactory::makeCapFloorEngine(
    const quantra::ModelSpec* model,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const OptionletVolEntry& volEntry) const
{
    // =========================================================================
    // Validate model
    // =========================================================================
    if (!model || !model->id()) {
        QUANTRA_ERROR("CapFloor model is missing or has no id");
    }
    std::string modelId = model->id()->str();

    if (model->payload_type() != quantra::ModelPayload_CapFloorModelSpec) {
        QUANTRA_ERROR("Model '" + modelId + "' is not a CapFloorModelSpec "
                      "(got payload_type=" + std::to_string(model->payload_type()) + ")");
    }

    auto* spec = model->payload_as_CapFloorModelSpec();
    if (!spec) {
        QUANTRA_ERROR("Model '" + modelId + "' has null CapFloorModelSpec payload");
    }
    
    auto modelType = spec->model_type();

    // =========================================================================
    // Validate model/vol compatibility and create engine
    // =========================================================================
    switch (modelType) {
        case quantra::enums::IrModelType_Bachelier:
            if (volEntry.qlVolType != QuantLib::Normal) {
                QUANTRA_ERROR("Model '" + modelId + "': Bachelier requires Normal vols, "
                              "but vol has type ShiftedLognormal");
            }
            return std::make_shared<QuantLib::BachelierCapFloorEngine>(
                discountCurve, volEntry.handle);

        case quantra::enums::IrModelType_Black:
            if (volEntry.displacement != 0.0) {
                QUANTRA_ERROR("Model '" + modelId + "': Black requires displacement=0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            return std::make_shared<QuantLib::BlackCapFloorEngine>(
                discountCurve, volEntry.handle);

        case quantra::enums::IrModelType_ShiftedBlack:
            if (volEntry.displacement <= 0.0) {
                QUANTRA_ERROR("Model '" + modelId + "': ShiftedBlack requires displacement>0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            // Note: BlackCapFloorEngine reads displacement from the vol structure
            return std::make_shared<QuantLib::BlackCapFloorEngine>(
                discountCurve, volEntry.handle);

        default:
            QUANTRA_ERROR("Model '" + modelId + "': Unknown IrModelType value " 
                          + std::to_string(modelType));
    }
    
    // Unreachable - QUANTRA_ERROR throws, but some compilers warn without this
    return nullptr;
}

std::shared_ptr<QuantLib::PricingEngine> EngineFactory::makeSwaptionEngine(
    const quantra::ModelSpec* model,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const SwaptionVolEntry& volEntry) const
{
    // =========================================================================
    // Validate model
    // =========================================================================
    if (!model || !model->id()) {
        QUANTRA_ERROR("Swaption model is missing or has no id");
    }
    std::string modelId = model->id()->str();

    if (model->payload_type() != quantra::ModelPayload_SwaptionModelSpec) {
        QUANTRA_ERROR("Model '" + modelId + "' is not a SwaptionModelSpec "
                      "(got payload_type=" + std::to_string(model->payload_type()) + ")");
    }

    auto* spec = model->payload_as_SwaptionModelSpec();
    if (!spec) {
        QUANTRA_ERROR("Model '" + modelId + "' has null SwaptionModelSpec payload");
    }
    
    auto modelType = spec->model_type();

    // =========================================================================
    // Validate model/vol compatibility and create engine
    // =========================================================================
    switch (modelType) {
        case quantra::enums::IrModelType_Bachelier:
#if QL_HAS_BACHELIER_SWAPTION_ENGINE
            if (volEntry.qlVolType != QuantLib::Normal) {
                QUANTRA_ERROR("Model '" + modelId + "': Bachelier requires Normal vols");
            }
            return std::make_shared<QuantLib::BachelierSwaptionEngine>(
                discountCurve, volEntry.handle);
#else
            QUANTRA_ERROR("Model '" + modelId + "': BachelierSwaptionEngine not available "
                          "in this QuantLib version (requires QuantLib 1.20+)");
#endif

        case quantra::enums::IrModelType_Black:
            if (volEntry.displacement != 0.0) {
                QUANTRA_ERROR("Model '" + modelId + "': Black requires displacement=0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            return std::make_shared<QuantLib::BlackSwaptionEngine>(
                discountCurve, volEntry.handle);

        case quantra::enums::IrModelType_ShiftedBlack:
            if (volEntry.displacement <= 0.0) {
                QUANTRA_ERROR("Model '" + modelId + "': ShiftedBlack requires displacement>0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            // Note: BlackSwaptionEngine reads displacement from the vol structure
            // (baked into ConstantSwaptionVolatility). Do NOT pass displacement to engine.
            return std::make_shared<QuantLib::BlackSwaptionEngine>(
                discountCurve, volEntry.handle);

        default:
            QUANTRA_ERROR("Model '" + modelId + "': Unknown IrModelType value "
                          + std::to_string(modelType));
    }
    
    // Unreachable - QUANTRA_ERROR throws, but some compilers warn without this
    return nullptr;
}

} // namespace quantra
