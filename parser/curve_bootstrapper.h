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
#include "quote_registry.h"
#include "curve_registry.h"
#include "index_factory.h"

namespace quantra {

/**
 * Result of bootstrapping: a map of curve id -> RelinkableHandle.
 *
 * RelinkableHandles are stable: they're created once and shared with all
 * helpers that reference the curve. After bootstrapping, each handle is
 * linked to the real curve.
 */
struct BootstrappedCurves {
    std::unordered_map<std::string,
        std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>> handles;
};

/**
 * CurveBootstrapper
 *
 * Orchestrates multi-curve bootstrapping with dependency resolution:
 *
 * 1. Creates empty RelinkableHandles for all curves
 * 2. Scans helper deps to build a dependency graph
 * 3. Topologically sorts curves
 * 4. Bootstraps in order, linking handles as each curve completes
 *
 * This enables scenarios like:
 *   - EUR OIS (ESTR) curve bootstrapped first (no deps)
 *   - EUR 6M (Euribor) curve bootstrapped second, using OIS as discount
 */
class CurveBootstrapper {
public:
    BootstrappedCurves bootstrapAll(
        const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>>* curves,
        const flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>* quotes = nullptr
    ) const;

private:
    /// Kahn's algorithm for topological sort
    static std::vector<std::string> topoSort(
        const std::unordered_map<std::string, std::vector<std::string>>& deps);

    /// Collect curve dependency edges from helper HelperDependencies
    static void collectDeps(
        const quantra::TermStructure* ts,
        std::unordered_map<std::string, std::vector<std::string>>& deps);
};

} // namespace quantra

#endif // QUANTRASERVER_CURVE_BOOTSTRAPPER_H
