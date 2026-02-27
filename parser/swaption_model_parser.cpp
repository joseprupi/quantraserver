#include "swaption_model_parser.h"

#include "error.h"

namespace quantra {

const quantra::SwaptionModelSpec* SwaptionModelParser::parse(
    const quantra::ModelSpec* model,
    const std::string& modelId) const {
    if (!model) {
        QUANTRA_ERROR("Model '" + modelId + "' not found");
    }
    if (model->payload_type() != quantra::ModelPayload_SwaptionModelSpec) {
        QUANTRA_ERROR("Model '" + modelId + "' is not a SwaptionModelSpec");
    }
    const auto* spec = model->payload_as_SwaptionModelSpec();
    if (!spec) {
        QUANTRA_ERROR("Model '" + modelId + "' has null SwaptionModelSpec payload");
    }
    return spec;
}

std::string SwaptionModelParser::extractTradeFloatIndexId(const quantra::Swaption* swaption) const {
    if (!swaption) {
        return {};
    }
    if (swaption->underlying_type() == quantra::SwaptionUnderlying_VanillaSwap) {
        const auto* u = swaption->underlying_as_VanillaSwap();
        if (u && u->floating_leg() && u->floating_leg()->index() && u->floating_leg()->index()->id()) {
            return u->floating_leg()->index()->id()->str();
        }
        return {};
    }
    if (swaption->underlying_type() == quantra::SwaptionUnderlying_OisSwap) {
        return {};
    }
    if (swaption->underlying_swap() && swaption->underlying_swap()->floating_leg() &&
        swaption->underlying_swap()->floating_leg()->index() &&
        swaption->underlying_swap()->floating_leg()->index()->id()) {
        return swaption->underlying_swap()->floating_leg()->index()->id()->str();
    }
    return {};
}

} // namespace quantra
