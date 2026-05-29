#include "calibrate_swaption_model_mapper.h"

#include "enums.h"
#include "error.h"
#include "model_generated.h"

namespace quantra {

namespace {

/// Mirror of the FB SwaptionHwCalibrationSpec table into its plain-domain
/// counterpart. Matches the conversion PricingRegistryBuilder performs for
/// the model's embedded hw_calibration block (periods via TimeUnitToQL, D16).
SwaptionHwCalibrationDomain toCalibrationDomain(
    const quantra::SwaptionHwCalibrationSpec* c) {
    SwaptionHwCalibrationDomain hw;
    if (c->swaption_vol_id()) hw.swaption_vol_id = c->swaption_vol_id()->str();
    if (c->discount_curve_id()) hw.discount_curve_id = c->discount_curve_id()->str();
    if (c->forwarding_curve_id()) hw.forwarding_curve_id = c->forwarding_curve_id()->str();
    if (c->swap_index_id()) hw.swap_index_id = c->swap_index_id()->str();
    if (c->expiries()) {
        for (auto pit = c->expiries()->begin(); pit != c->expiries()->end(); ++pit) {
            hw.expiries.emplace_back(pit->n(), TimeUnitToQL(pit->unit()));
        }
    }
    if (c->tenors()) {
        for (auto pit = c->tenors()->begin(); pit != c->tenors()->end(); ++pit) {
            hw.tenors.emplace_back(pit->n(), TimeUnitToQL(pit->unit()));
        }
    }
    hw.calibrate_a = c->calibrate_a();
    hw.calibrate_sigma = c->calibrate_sigma();
    hw.a_init = c->a_init();
    hw.sigma_init = c->sigma_init();
    hw.max_iterations = c->max_iterations();
    hw.function_evaluations = c->function_evaluations();
    hw.end_criteria_eps = c->end_criteria_eps();
    return hw;
}

} // namespace

CalibrateSwaptionModelInputs CalibrateSwaptionModelMapper::toInputs(
    const quantra::CalibrateSwaptionModelRequest* req) const {
    if (req == nullptr || req->pricing() == nullptr || req->model_id() == nullptr) {
        QUANTRA_INVALID_ARGUMENT(
            "CalibrateSwaptionModelRequest requires pricing and model_id");
    }

    CalibrateSwaptionModelInputs inputs;
    inputs.modelId = req->model_id()->str();

    // The mapper has no registry at toInputs time, so the referenced model is
    // read straight from pricing.models[] — the same source the registry
    // builds from. The model_type guard runs regardless of whether the request
    // supplies a calibration override, matching the legacy path.
    const quantra::SwaptionModelSpec* modelSpec = nullptr;
    const auto* volatility = req->pricing()->volatility();
    if (const auto* models = volatility ? volatility->models() : nullptr) {
        for (auto it = models->begin(); it != models->end(); ++it) {
            if (it->id() && it->id()->str() == inputs.modelId) {
                if (it->payload_type() != quantra::ModelPayload_SwaptionModelSpec) {
                    QUANTRA_INVALID_ARGUMENT(
                        "Model '" + inputs.modelId + "' is not a SwaptionModelSpec");
                }
                modelSpec = it->payload_as_SwaptionModelSpec();
                break;
            }
        }
    }
    if (modelSpec == nullptr) {
        QUANTRA_NOT_FOUND("Model not found: " + inputs.modelId);
    }
    if (modelSpec->model_type() != quantra::enums::IrModelType_HullWhiteLattice) {
        QUANTRA_INVALID_ARGUMENT(
            "Model '" + inputs.modelId + "' must be HullWhiteLattice for calibration");
    }

    // Calibration source: request override wins; else fall back to the
    // referenced model's hw_calibration block (legacy fallback preserved).
    const quantra::SwaptionHwCalibrationSpec* calibSpec = req->calibration();
    if (calibSpec == nullptr) {
        calibSpec = modelSpec->hw_calibration();
    }
    if (calibSpec == nullptr) {
        QUANTRA_INVALID_ARGUMENT(
            "Calibration spec is required (request.calibration or model.hw_calibration)");
    }

    inputs.calibration = toCalibrationDomain(calibSpec);
    return inputs;
}

flatbuffers::Offset<quantra::CalibrateSwaptionModelResponse>
CalibrateSwaptionModelMapper::toResponse(
    flatbuffers::grpc::MessageBuilder& builder,
    const CalibrateSwaptionModelResult& result) const {
    auto modelIdStr = builder.CreateString(result.modelId);
    quantra::CalibrateSwaptionModelResponseBuilder rb(builder);
    rb.add_model_id(modelIdStr);
    rb.add_hw_a(result.a);
    rb.add_hw_sigma(result.sigma);
    rb.add_rmse(result.rmse);
    rb.add_num_helpers(result.numHelpers);
    rb.add_grid_rows(result.gridRows);
    rb.add_grid_cols(result.gridCols);
    rb.add_grid_points(result.gridPoints);
    return rb.Finish();
}

} // namespace quantra
