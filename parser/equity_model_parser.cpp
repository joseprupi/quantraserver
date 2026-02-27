#include "equity_model_parser.h"

#include "error.h"

namespace quantra {

const quantra::EquityVanillaModelSpec* EquityModelParser::parse(
    const quantra::ModelSpec* model,
    const std::string& modelId) const {
    if (!model) {
        QUANTRA_ERROR("Model not found: " + modelId);
    }
    if (model->payload_type() != quantra::ModelPayload_EquityVanillaModelSpec) {
        QUANTRA_ERROR("Model '" + modelId + "' is not an EquityVanillaModelSpec");
    }
    const auto* parsed = model->payload_as_EquityVanillaModelSpec();
    if (!parsed) {
        QUANTRA_ERROR("Model '" + modelId + "' EquityVanillaModelSpec payload is null");
    }
    return parsed;
}

} // namespace quantra
