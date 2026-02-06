#ifndef QUANTRASERVER_CURVE_BOOTSTRAPPER_H
#define QUANTRASERVER_CURVE_BOOTSTRAPPER_H

#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

#include <ql/handle.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>

#include "term_structure_generated.h"
#include "quotes_generated.h"
#include "index_generated.h"
#include "quote_registry.h"
#include "curve_registry.h"
#include "index_registry.h"
#include "index_registry_builder.h"

namespace quantra {

/**
 * Result of bootstrapping: a map of curve id -> RelinkableHandle.
 */
struct BootstrappedCurves {
    std::unordered_map<std::string,
        std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>> handles;
};

/**
 * CurveBootstrapper
 *
 * Orchestrates multi-curve bootstrapping with dependency resolution.
 * Now accepts an IndexRegistry for resolving IndexRef in helpers.
 */
class CurveBootstrapper {
public:
    BootstrappedCurves bootstrapAll(
        const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>>* curves,
        const flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>* quotes = nullptr,
        const flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>* indices = nullptr
    ) const;

    static std::vector<std::string> topoSort(
        const std::unordered_map<std::string, std::vector<std::string>>& deps);

    static void collectDeps(
        const quantra::TermStructure* ts,
        std::unordered_map<std::string, std::vector<std::string>>& deps);
};

} // namespace quantra

#endif // QUANTRASERVER_CURVE_BOOTSTRAPPER_H
