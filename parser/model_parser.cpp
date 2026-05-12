#include "model_parser.h"

namespace quantra {

void ModelParser::registerModel(const quantra::ModelSpec* spec,
                                std::map<std::string, const quantra::ModelSpec*>& out) const {
    if (!spec) QUANTRA_INVALID_ARGUMENT("ModelSpec not found");
    if (!spec->id()) QUANTRA_INVALID_ARGUMENT("ModelSpec.id required");

    std::string id = spec->id()->str();
    if (spec->payload_type() == quantra::ModelPayload_NONE) {
        QUANTRA_INVALID_ARGUMENT("ModelSpec.payload required for model id: " + id);
    }
    out[id] = spec;
}

} // namespace quantra