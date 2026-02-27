#ifndef QUANTRA_EQUITY_MODEL_PARSER_H
#define QUANTRA_EQUITY_MODEL_PARSER_H

#include <string>

#include "model_generated.h"

namespace quantra {

class EquityModelParser {
public:
    const quantra::EquityVanillaModelSpec* parse(const quantra::ModelSpec* model, const std::string& modelId) const;
};

} // namespace quantra

#endif // QUANTRA_EQUITY_MODEL_PARSER_H
