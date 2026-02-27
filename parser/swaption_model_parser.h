#ifndef QUANTRA_SWPTION_MODEL_PARSER_H
#define QUANTRA_SWPTION_MODEL_PARSER_H

#include <string>

#include "model_generated.h"
#include "swaption_generated.h"

namespace quantra {

class SwaptionModelParser {
public:
    const quantra::SwaptionModelSpec* parse(const quantra::ModelSpec* model, const std::string& modelId) const;
    std::string extractTradeFloatIndexId(const quantra::Swaption* swaption) const;
};

} // namespace quantra

#endif // QUANTRA_SWPTION_MODEL_PARSER_H
