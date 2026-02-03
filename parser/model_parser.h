#ifndef QUANTRASERVER_MODEL_PARSER_H
#define QUANTRASERVER_MODEL_PARSER_H

#include <map>
#include <string>

#include "model_generated.h"
#include "common.h"

namespace quantra {

class ModelParser {
public:
    void registerModel(const quantra::ModelSpec* spec,
                       std::map<std::string, const quantra::ModelSpec*>& out) const;
};

} // namespace quantra

#endif