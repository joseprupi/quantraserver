#include "curve_bootstrapper.h"

#include <queue>
#include <algorithm>
#include <chrono>

#include "common.h"
#include "term_structure_parser.h"
#include "curve_cache.h"
#include "curve_cache_key.h"
#include "curve_serializer.h"

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

/// Add only discount_curve dependency.
/// Used for SwapHelper/OISHelper/DatedOISHelper where QuantLib overrides the
/// projection curve with the curve being bootstrapped (via setTermStructure).
/// projection_curve is validated and rejected later in TermStructurePointParser;
/// we must not add it as a dependency here or the topo-sort may produce a
/// misleading cycle error before the parser can emit the clear hard-error.
static void addDiscountOnlyDep(
    std::unordered_map<std::string, std::vector<std::string>>& deps,
    const std::string& curveId,
    const quantra::HelperDependencies* hd)
{
    if (!hd) return;
    addDepFromRef(deps, curveId, hd->discount_curve());
}

/// Add all dependency edges (discount + projection + projection_2).
/// Used for helpers that genuinely encode multi-curve projection relationships:
/// TenorBasisSwapHelper, FxSwapHelper, CrossCcyBasisHelper.
static void addAllDeps(
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

    deps.try_emplace(curveId, std::vector<std::string>{});

    if (!ts->points()) return;

    for (flatbuffers::uoffset_t i = 0; i < ts->points()->size(); i++) {
        auto wrapper = ts->points()->Get(i);
        auto ptype = wrapper->point_type();

        // Swap/OIS helpers: discount-curve only.
        // QuantLib's SwapRateHelper/OISRateHelper override projection via
        // index->clone(termStructureHandle_), so projection_curve deps are
        // meaningless and would create bogus ordering constraints.
        if (ptype == quantra::Point_SwapHelper) {
            auto h = static_cast<const quantra::SwapHelper*>(wrapper->point());
            addDiscountOnlyDep(deps, curveId, h->deps());
        }
        else if (ptype == quantra::Point_OISHelper) {
            auto h = static_cast<const quantra::OISHelper*>(wrapper->point());
            addDiscountOnlyDep(deps, curveId, h->deps());
        }
        else if (ptype == quantra::Point_DatedOISHelper) {
            auto h = static_cast<const quantra::DatedOISHelper*>(wrapper->point());
            addDiscountOnlyDep(deps, curveId, h->deps());
        }
        // Basis/XCCY/FX helpers: full deps including projection curves.
        // These helpers genuinely use exogenous projection relationships.
        else if (ptype == quantra::Point_TenorBasisSwapHelper) {
            auto h = static_cast<const quantra::TenorBasisSwapHelper*>(wrapper->point());
            addAllDeps(deps, curveId, h->deps());
        }
        else if (ptype == quantra::Point_FxSwapHelper) {
            auto h = static_cast<const quantra::FxSwapHelper*>(wrapper->point());
            addAllDeps(deps, curveId, h->deps());
        }
        else if (ptype == quantra::Point_CrossCcyBasisHelper) {
            auto h = static_cast<const quantra::CrossCcyBasisHelper*>(wrapper->point());
            addAllDeps(deps, curveId, h->deps());
        }
    }
}

// =============================================================================
// Topological sort (Kahn's algorithm)
// =============================================================================

std::vector<std::string> CurveBootstrapper::topoSort(
    const std::unordered_map<std::string, std::vector<std::string>>& deps)
{
    std::unordered_map<std::string, int> indeg;
    std::unordered_map<std::string, std::vector<std::string>> adj;

    for (const auto& kv : deps) {
        indeg.try_emplace(kv.first, 0);
        for (const auto& d : kv.second) {
            indeg.try_emplace(d, 0);
        }
    }

    for (const auto& kv : deps) {
        const auto& u = kv.first;
        for (const auto& v : kv.second) {
            adj[v].push_back(u);
            indeg[u] += 1;
        }
    }

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
        QUANTRA_ERROR("Curve dependency graph has a cycle.");
    }

    return order;
}

// =============================================================================
// Bootstrap all curves in dependency order
// =============================================================================

BootstrappedCurves CurveBootstrapper::bootstrapAll(
    const flatbuffers::Vector<flatbuffers::Offset<quantra::TermStructure>>* curves,
    const flatbuffers::Vector<flatbuffers::Offset<quantra::QuoteSpec>>* quotes,
    const flatbuffers::Vector<flatbuffers::Offset<quantra::IndexDef>>* indices,
    double curveBump
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
            double v = q->value();
            if (q->quote_type() == quantra::QuoteType_Curve) {
                v += curveBump;
            }
            quoteReg.upsert(q->id()->str(), v, q->quote_type());
        }
    }

    // ---- 2. Build IndexRegistry ----
    IndexRegistryBuilder indexBuilder;
    IndexRegistry indexReg = indexBuilder.build(indices);

    // ---- 3. Index curves by id ----
    std::unordered_map<std::string, const quantra::TermStructure*> curveIndex;
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); i++) {
        auto ts = curves->Get(i);
        if (!ts->id()) QUANTRA_ERROR("TermStructure.id is required for multi-curve bootstrapping");
        curveIndex[ts->id()->str()] = ts;
    }

    // ---- 4. Create empty handles and register them ----
    BootstrappedCurves out;
    CurveRegistry curveReg;

    for (const auto& kv : curveIndex) {
        auto h = std::make_shared<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>();
        out.handles.emplace(kv.first, h);
        curveReg.put(kv.first, *h);
    }

    // ---- 5. Build dependency graph ----
    std::unordered_map<std::string, std::vector<std::string>> deps;
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); i++) {
        collectDeps(curves->Get(i), deps);
    }

    // ---- 6. Topological sort ----
    auto order = topoSort(deps);

    // ---- 7. Bootstrap in order (with cache) ----
    TermStructureParser tsParser;
    auto& cache = CurveCache::instance();
    std::map<std::string, std::string> depKeys; // curve_id → cache_key (for dep chaining)

    // Build quote/index lookup maps once (O(1) lookups during key computation)
    KeyContext keyCtx;
    bool useCache = cache.enabled() && curveBump == 0.0;
    if (useCache) {
        keyCtx = KeyContext::build(quotes, indices);
    }

    for (const auto& id : order) {
        auto it = curveIndex.find(id);
        if (it == curveIndex.end()) {
            if (curveReg.has(id)) continue;
            QUANTRA_ERROR("Curve id '" + id + "' referenced in dependencies but not provided");
        }

        const auto* ts = it->second;

        if (useCache) {
            auto t0 = std::chrono::steady_clock::now();

            // Compute canonical cache key
            // depKeys contains keys of already-resolved dependency curves
            std::map<std::string, std::string> relevantDepKeys;
            if (deps.count(id)) {
                for (const auto& depId : deps.at(id)) {
                    if (depKeys.count(depId)) {
                        relevantDepKeys[depId] = depKeys.at(depId);
                    }
                }
            }

            std::string asOfDate;
            auto evalDate = QuantLib::Settings::instance().evaluationDate();
            std::ostringstream os;
            os << QuantLib::io::iso_date(evalDate);
            asOfDate = os.str();

            std::string key = CurveKeyBuilder::compute(
                asOfDate, ts, keyCtx, relevantDepKeys);

            auto tKey = std::chrono::steady_clock::now();
            double keyMs = std::chrono::duration<double, std::milli>(tKey - t0).count();

            // --- L1 check ---
            auto cached = cache.backend().getL1(key);
            if (cached) {
                cache.stats().l1_hits++;
                cache.logEvent(id, key, "L1_HIT", keyMs);
                out.handles.at(id)->linkTo(cached);
                depKeys[id] = key;
                continue;
            }
            cache.stats().l1_misses++;

            // --- L2 check (future: Redis) ---
            auto l2data = cache.backend().getL2(key);
            if (l2data.has_value()) {
                cache.stats().l2_hits++;
                auto curve = CurveSerializer::reconstruct(l2data.value());
                cache.backend().putL1(key, curve);
                cache.logEvent(id, key, "L2_HIT");
                out.handles.at(id)->linkTo(curve);
                depKeys[id] = key;
                continue;
            }
            cache.stats().l2_misses++;

            // --- Full bootstrap (cache miss) ---
            auto tBootStart = std::chrono::steady_clock::now();
            auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexReg, curveBump);
            auto tBootEnd = std::chrono::steady_clock::now();
            double bootMs = std::chrono::duration<double, std::milli>(tBootEnd - tBootStart).count();

            cache.stats().bootstraps++;

            // Store in L1
            cache.backend().putL1(key, curve);

            // Store in L2 (serialized DFs — for future Redis)
            auto serialized = CurveSerializer::serialize(curve, ts);
            cache.backend().putL2(key, serialized);

            cache.logEvent(id, key, "MISS_BOOTSTRAP", bootMs);

            out.handles.at(id)->linkTo(curve);
            depKeys[id] = key;
        } else {
            // Cache disabled — original behavior
            auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexReg, curveBump);
            out.handles.at(id)->linkTo(curve);
        }
    }

    return out;
}

} // namespace quantra
