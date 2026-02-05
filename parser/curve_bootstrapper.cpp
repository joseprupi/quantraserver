#include "curve_bootstrapper.h"

#include <queue>
#include <algorithm>

#include "common.h"
#include "term_structure_parser.h"

namespace quantra {

// =============================================================================
// Helpers
// =============================================================================

static void addDep(
    std::unordered_map<std::string, std::vector<std::string>>& deps,
    const std::string& from,
    const std::string& to)
{
    if (to.empty() || from == to) return;
    deps[from].push_back(to);
}

static void addDepFromRef(
    std::unordered_map<std::string, std::vector<std::string>>& deps,
    const std::string& curveId,
    const quantra::CurveRef* ref)
{
    if (ref && ref->id()) {
        addDep(deps, curveId, ref->id()->str());
    }
}

static void addDepsFromHelper(
    std::unordered_map<std::string, std::vector<std::string>>& deps,
    const std::string& curveId,
    const quantra::HelperDependencies* hd)
{
    if (!hd) return;
    addDepFromRef(deps, curveId, hd->discount_curve());
    addDepFromRef(deps, curveId, hd->projection_curve());
    addDepFromRef(deps, curveId, hd->projection_curve_2());
}

// =============================================================================
// Collect dependencies from all helpers in a curve
// =============================================================================

void CurveBootstrapper::collectDeps(
    const quantra::TermStructure* ts,
    std::unordered_map<std::string, std::vector<std::string>>& deps)
{
    if (!ts->id()) return;
    std::string curveId = ts->id()->str();

    // Ensure node exists even with no deps
    deps.try_emplace(curveId, std::vector<std::string>{});

    if (!ts->points()) return;

    for (flatbuffers::uoffset_t i = 0; i < ts->points()->size(); i++) {
        auto wrapper = ts->points()->Get(i);
        auto ptype = wrapper->point_type();

        // SwapHelper has deps
        if (ptype == quantra::Point_SwapHelper) {
            auto h = static_cast<const quantra::SwapHelper*>(wrapper->point());
            addDepsFromHelper(deps, curveId, h->deps());
        }
        // TenorBasisSwapHelper has deps
        else if (ptype == quantra::Point_TenorBasisSwapHelper) {
            auto h = static_cast<const quantra::TenorBasisSwapHelper*>(wrapper->point());
            addDepsFromHelper(deps, curveId, h->deps());
        }
        // FxSwapHelper has deps
        else if (ptype == quantra::Point_FxSwapHelper) {
            auto h = static_cast<const quantra::FxSwapHelper*>(wrapper->point());
            addDepsFromHelper(deps, curveId, h->deps());
        }
        // CrossCcyBasisHelper has deps
        else if (ptype == quantra::Point_CrossCcyBasisHelper) {
            auto h = static_cast<const quantra::CrossCcyBasisHelper*>(wrapper->point());
            addDepsFromHelper(deps, curveId, h->deps());
        }
        // Other helpers (Deposit, FRA, Future, Bond, OIS, DatedOIS)
        // have no exogenous curve deps
    }
}

// =============================================================================
// Topological sort (Kahn's algorithm)
// =============================================================================

std::vector<std::string> CurveBootstrapper::topoSort(
    const std::unordered_map<std::string, std::vector<std::string>>& deps)
{
    // Build adjacency list and in-degree map
    // Edge: u -> v means "u depends on v" (v must be built before u)
    // So in the adjacency list for the sort, v -> u (v enables u)
    std::unordered_map<std::string, int> indeg;
    std::unordered_map<std::string, std::vector<std::string>> adj; // v -> [u1, u2, ...]

    // Initialize all nodes
    for (const auto& kv : deps) {
        indeg.try_emplace(kv.first, 0);
        for (const auto& d : kv.second) {
            indeg.try_emplace(d, 0);
        }
    }

    // Build edges: if u depends on v, then v -> u
    for (const auto& kv : deps) {
        const auto& u = kv.first;
        for (const auto& v : kv.second) {
            adj[v].push_back(u);
            indeg[u] += 1;
        }
    }

    // Kahn's: start with nodes that have no dependencies
    std::queue<std::string> q;
    for (const auto& kv : indeg) {
        if (kv.second == 0) q.push(kv.first);
    }

    std::vector<std::string> order;
    order.reserve(indeg.size());

    while (!q.empty()) {
        auto node = q.front();
        q.pop();
        order.push_back(node);

        for (const auto& dependent : adj[node]) {
            if (--indeg[dependent] == 0) {
                q.push(dependent);
            }
        }
    }

    if (order.size() != indeg.size()) {
        QUANTRA_ERROR("Curve dependency graph has a cycle. "
                      "Check that no two curves depend on each other.");
    }

    return order;
}

// =============================================================================
// Bootstrap all curves in dependency order
// =============================================================================

BootstrappedCurves CurveBootstrapper::bootstrapAll(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>>* curves,
    const flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>* quotes
) const {
    if (!curves || curves->size() == 0) {
        QUANTRA_ERROR("curves is required (at least one curve)");
    }

    // ---- 1. Build QuoteRegistry ----
    QuoteRegistry quoteReg;
    if (quotes) {
        for (flatbuffers::uoffset_t i = 0; i < quotes->size(); i++) {
            auto q = quotes->Get(i);
            if (!q->id()) QUANTRA_ERROR("QuoteSpec.id is required");
            quoteReg.upsert(q->id()->str(), q->value());
        }
    }

    // ---- 2. Index curves by id ----
    std::unordered_map<std::string, const quantra::TermStructure*> curveIndex;
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); i++) {
        auto ts = curves->Get(i);
        if (!ts->id()) QUANTRA_ERROR("TermStructure.id is required for multi-curve bootstrapping");
        curveIndex[ts->id()->str()] = ts;
    }

    // ---- 3. Create empty handles and register them ----
    BootstrappedCurves out;
    CurveRegistry curveReg;

    for (const auto& kv : curveIndex) {
        auto h = std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>();
        out.handles.emplace(kv.first, h);
        // Register the empty handle — helpers will hold a copy of this Handle
        // and see the real curve once we linkTo() after bootstrapping
        curveReg.put(kv.first, *h);
    }

    // ---- 4. Build dependency graph ----
    std::unordered_map<std::string, std::vector<std::string>> deps;
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); i++) {
        collectDeps(curves->Get(i), deps);
    }

    // ---- 5. Topological sort ----
    auto order = topoSort(deps);

    // ---- 6. Bootstrap in order ----
    IndexFactory indexFactory;
    TermStructureParser tsParser;

    for (const auto& id : order) {
        auto it = curveIndex.find(id);
        if (it == curveIndex.end()) {
            // This id was referenced as a dependency but not provided
            // It might be an external/pre-existing curve — skip if already in registry
            if (curveReg.has(id)) continue;
            QUANTRA_ERROR("Curve id '" + id + "' referenced in dependencies but not provided");
        }

        const auto* ts = it->second;
        auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexFactory);

        // Link the handle so all downstream helpers see the real curve
        out.handles.at(id)->linkTo(curve);
    }

    return out;
}

} // namespace quantra
