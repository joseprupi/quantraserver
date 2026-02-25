#include "calibrate_swaption_model_pricing_request.h"

#include <ql/settings.hpp>

#include "common.h"
#include "error.h"
#include "pricing_registry.h"
#include "swaption_model_calibration.h"

flatbuffers::Offset<quantra::CalibrateSwaptionModelResponse>
CalibrateSwaptionModelPricingRequest::request(
    std::shared_ptr<flatbuffers::grpc::MessageBuilder> builder,
    const quantra::CalibrateSwaptionModelRequest* request) const {
    if (!request || !request->pricing() || !request->model_id()) {
        QUANTRA_ERROR("CalibrateSwaptionModelRequest requires pricing and model_id");
    }

    quantra::PricingRegistryBuilder regBuilder;
    quantra::PricingRegistry reg = regBuilder.build(request->pricing());
    const QuantLib::Date asOf = DateToQL(request->pricing()->as_of_date()->str());
    QuantLib::Settings::instance().evaluationDate() = asOf;

    const std::string modelId = request->model_id()->str();
    auto mIt = reg.models.find(modelId);
    if (mIt == reg.models.end()) {
        QUANTRA_ERROR("Model not found: " + modelId);
    }
    const auto* model = mIt->second;
    if (model->payload_type() != quantra::ModelPayload_SwaptionModelSpec) {
        QUANTRA_ERROR("Model '" + modelId + "' is not a SwaptionModelSpec");
    }
    const auto* spec = model->payload_as_SwaptionModelSpec();
    if (!spec) {
        QUANTRA_ERROR("Model '" + modelId + "' has null SwaptionModelSpec payload");
    }
    if (spec->model_type() != quantra::enums::IrModelType_HullWhiteLattice) {
        QUANTRA_ERROR("Model '" + modelId + "' must be HullWhiteLattice for calibration");
    }

    const quantra::SwaptionHwCalibrationSpec* calibSpec = request->calibration();
    if (!calibSpec) {
        calibSpec = spec->hw_calibration();
    }
    if (!calibSpec) {
        QUANTRA_ERROR("Calibration spec is required (request.calibration or model.hw_calibration)");
    }

    auto result = quantra::calibrateHullWhiteFromSwaptionVol(reg, calibSpec, asOf);

    auto modelIdStr = builder->CreateString(modelId);
    quantra::CalibrateSwaptionModelResponseBuilder rb(*builder);
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
