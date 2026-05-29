/**
 * Engine Factory Implementation
 * 
 * Creates pricing engines with model/vol validation.
 */

#include "engine_factory.h"

#include <ql/pricingengines/capfloor/bacheliercapfloorengine.hpp>
#include <ql/pricingengines/capfloor/blackcapfloorengine.hpp>
#include <ql/pricingengines/swaption/blackswaptionengine.hpp>
#include <ql/pricingengines/swaption/treeswaptionengine.hpp>
#include <ql/models/shortrate/onefactormodels/hullwhite.hpp>
#include <ql/version.hpp>

// BachelierSwaptionEngine is defined in blackswaptionengine.hpp for the
// QuantLib versions we support. Gate on version instead of header presence.
#if QL_HEX_VERSION >= 0x012000f0
    #define QL_HAS_BACHELIER_SWAPTION_ENGINE 1
#else
    #define QL_HAS_BACHELIER_SWAPTION_ENGINE 0
#endif

namespace quantra {

std::shared_ptr<QuantLib::PricingEngine> EngineFactory::makeHullWhiteLatticeSwaptionEngine(
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    double hwA,
    double hwSigma,
    int latticeSteps) const {
    if (latticeSteps <= 0) {
        QUANTRA_INVALID_ARGUMENT("HullWhiteLattice requires lattice_steps > 0");
    }
    auto hwModel = std::make_shared<QuantLib::HullWhite>(discountCurve, hwA, hwSigma);
    return std::make_shared<QuantLib::TreeSwaptionEngine>(hwModel, latticeSteps);
}

std::shared_ptr<QuantLib::PricingEngine> EngineFactory::makeCapFloorEngine(
    const quantra::ModelSpec* model,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& discountCurve,
    const OptionletVolEntry& volEntry) const
{
    // =========================================================================
    // Validate model
    // =========================================================================
    if (!model || !model->id()) {
        QUANTRA_INVALID_ARGUMENT("CapFloor model is missing or has no id");
    }
    std::string modelId = model->id()->str();

    if (model->payload_type() != quantra::ModelPayload_CapFloorModelSpec) {
        QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "' is not a CapFloorModelSpec "
                      "(got payload_type=" + std::to_string(model->payload_type()) + ")");
    }

    auto* spec = model->payload_as_CapFloorModelSpec();
    if (!spec) {
        QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "' has null CapFloorModelSpec payload");
    }
    
    auto modelType = spec->model_type();

    // =========================================================================
    // Validate model/vol compatibility and create engine
    // =========================================================================
    switch (modelType) {
        case quantra::enums::IrModelType_Bachelier:
            if (volEntry.qlVolType != QuantLib::Normal) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Bachelier requires Normal vols, "
                              "but vol has type ShiftedLognormal");
            }
            return std::make_shared<QuantLib::BachelierCapFloorEngine>(
                discountCurve, volEntry.handle);

        case quantra::enums::IrModelType_Black:
            if (volEntry.displacement != 0.0) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Black requires displacement=0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            return std::make_shared<QuantLib::BlackCapFloorEngine>(
                discountCurve, volEntry.handle);

        case quantra::enums::IrModelType_ShiftedBlack:
            if (volEntry.displacement <= 0.0) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': ShiftedBlack requires displacement>0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            // Note: BlackCapFloorEngine reads displacement from the vol structure
            return std::make_shared<QuantLib::BlackCapFloorEngine>(
                discountCurve, volEntry.handle);

        default:
            QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Unknown IrModelType value " 
                          + std::to_string(modelType));
    }
    
    // Unreachable - the throw above exits, but some compilers warn without this
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
        QUANTRA_INVALID_ARGUMENT("Swaption model is missing or has no id");
    }
    std::string modelId = model->id()->str();

    if (model->payload_type() != quantra::ModelPayload_SwaptionModelSpec) {
        QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "' is not a SwaptionModelSpec "
                      "(got payload_type=" + std::to_string(model->payload_type()) + ")");
    }

    auto* spec = model->payload_as_SwaptionModelSpec();
    if (!spec) {
        QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "' has null SwaptionModelSpec payload");
    }
    
    auto modelType = spec->model_type();

    // =========================================================================
    // SABR vol kinds return Black (lognormal) vols via the Hagan expansion;
    // pairing them with a Bachelier (Normal) engine is a configuration error.
    // Reject early with a clear message, regardless of the underlying QL volatility
    // type (which for SABR cubes is always set to ShiftedLognormal).
    // =========================================================================
    if (modelType == quantra::enums::IrModelType_Bachelier &&
        (volEntry.volKind == quantra::enums::SwaptionVolKind_SabrParams ||
         volEntry.volKind == quantra::enums::SwaptionVolKind_SabrCalibrate)) {
        QUANTRA_INVALID_ARGUMENT(
            "Model '" + modelId + "': Bachelier engine cannot be paired with SABR vol surface "
            "(SABR via Hagan returns lognormal Black vol). Use Black or ShiftedBlack instead.");
    }

    // =========================================================================
    // Validate model/vol compatibility and create engine
    // =========================================================================
    switch (modelType) {
        case quantra::enums::IrModelType_Bachelier:
#if QL_HAS_BACHELIER_SWAPTION_ENGINE
            if (volEntry.qlVolType != QuantLib::Normal) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Bachelier requires Normal vols");
            }
            return std::make_shared<QuantLib::BachelierSwaptionEngine>(
                discountCurve, volEntry.handle);
#else
            QUANTRA_NOT_IMPLEMENTED("Model '" + modelId + "': BachelierSwaptionEngine not available "
                          "in this QuantLib version (requires QuantLib 1.20+)");
#endif

        case quantra::enums::IrModelType_Black:
            if (volEntry.displacement != 0.0) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Black requires displacement=0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            return std::make_shared<QuantLib::BlackSwaptionEngine>(
                discountCurve, volEntry.handle);

        case quantra::enums::IrModelType_ShiftedBlack:
            if (volEntry.displacement <= 0.0) {
                QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': ShiftedBlack requires displacement>0, "
                              "but vol has displacement=" + std::to_string(volEntry.displacement));
            }
            // Note: BlackSwaptionEngine reads displacement from the vol structure
            // (baked into ConstantSwaptionVolatility). Do NOT pass displacement to engine.
            return std::make_shared<QuantLib::BlackSwaptionEngine>(
                discountCurve, volEntry.handle);

        case quantra::enums::IrModelType_HullWhiteLattice: {
            const double a = spec->hw_a();
            const double sigma = spec->hw_sigma();
            const int latticeSteps = spec->lattice_steps();
            return makeHullWhiteLatticeSwaptionEngine(discountCurve, a, sigma, latticeSteps);
        }

        default:
            QUANTRA_INVALID_ARGUMENT("Model '" + modelId + "': Unknown IrModelType value "
                          + std::to_string(modelType));
    }
    
    // Unreachable - the throw above exits, but some compilers warn without this
    return nullptr;
}

} // namespace quantra
