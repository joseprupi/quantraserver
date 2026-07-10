#include "curve_bootstrapper.h"

#include <queue>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>

#include "date_convert.h"
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
        QUANTRA_INVALID_ARGUMENT("Curve dependency graph has a cycle.");
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
    double curveBump,
    const RequestBudget& budget
) const {
    if (!curves || curves->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("curves is required (at least one curve)");
    }

    // ---- 1. Build QuoteRegistry ----
    QuoteRegistry quoteReg;
    if (quotes) {
        for (flatbuffers::uoffset_t i = 0; i < quotes->size(); i++) {
            auto q = quotes->Get(i);
            if (!q->id()) QUANTRA_INVALID_ARGUMENT("QuoteSpec.id is required");
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
        if (!ts->id()) QUANTRA_INVALID_ARGUMENT("TermStructure.id is required for multi-curve bootstrapping");
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
        // Honor the per-request deadline before bootstrapping each curve — the
        // heaviest step in the pricing path and the natural mid-computation
        // checkpoint. A default (unlimited) budget never triggers.
        budget.check();
        auto it = curveIndex.find(id);
        if (it == curveIndex.end()) {
            if (curveReg.has(id)) continue;
            QUANTRA_NOT_FOUND("Curve id '" + id + "' referenced in dependencies but not provided");
        }

        const auto* ts = it->second;

        if (useCache) {
            auto t0 = std::chrono::steady_clock::now();

            // Compute canonical cache key
            // depKeys contains keys of already-resolved dependency curves.
            // A dependency with no key was itself unkeyable; omitting its
            // identity from this curve's key would under-key it the same way,
            // so unkeyability propagates to dependents.
            bool keyable = true;
            std::map<std::string, std::string> relevantDepKeys;
            if (deps.count(id)) {
                for (const auto& depId : deps.at(id)) {
                    if (depKeys.count(depId)) {
                        relevantDepKeys[depId] = depKeys.at(depId);
                    } else {
                        keyable = false;
                    }
                }
            }

            std::string asOfDate;
            auto evalDate = QuantLib::Settings::instance().evaluationDate();
            std::ostringstream os;
            os << DateToIso(evalDate);
            asOfDate = os.str();

            std::string key;
            if (keyable) {
                try {
                    key = CurveKeyBuilder::compute(
                        asOfDate, ts, keyCtx, relevantDepKeys);
                } catch (const std::exception& e) {
                    keyable = false;
                    // Key-build failure must never fail pricing — it only
                    // loses caching for this curve. Warn once per process so
                    // the degradation is visible without flooding logs.
                    static std::atomic<bool> warned{false};
                    if (!warned.exchange(true)) {
                        std::cerr << "[CurveCache] WARNING: cache key build failed"
                                  << " for curve '" << id << "': " << e.what()
                                  << " — caching skipped for affected curves"
                                  << " (logged once per process)" << std::endl;
                    }
                }
            }
            if (!keyable) {
                // Fail closed: this curve is simply not cached — no L1/L2 get
                // or put, no depKeys entry. Bootstrap live; request succeeds.
                cache.logEvent(id, "", "KEY_BUILD_FAILED_UNCACHED");
                auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexReg, curveBump);
                out.handles.at(id)->linkTo(curve);
                continue;
            }

            auto tKey = std::chrono::steady_clock::now();
            double keyMs = std::chrono::duration<double, std::milli>(tKey - t0).count();

            // --- L1 check ---
            auto cached = cache.backend().getL1(key);
            if (cached) {
                cache.stats().l1_hits++;
                cache.logEvent(id, key, "L1_HIT", keyMs);
                out.handles.at(id)->linkTo(cached);
                depKeys[id] = key;
                out.keys[id] = key;
                continue;
            }
            cache.stats().l1_misses++;

            // --- L2 check (future: Redis) ---
            auto l2data = cache.backend().getL2(key);
            if (l2data.has_value()) {
                // reconstruct() fails closed on data it cannot rebuild exactly
                // (e.g. an unsupported interpolator); such an entry must not
                // serve a hit — treat it as a miss and bootstrap live.
                std::shared_ptr<QuantLib::YieldTermStructure> reconstructed;
                try {
                    reconstructed = CurveSerializer::reconstruct(l2data.value());
                } catch (const std::exception&) {
                    reconstructed = nullptr;
                }
                if (reconstructed) {
                    cache.stats().l2_hits++;
                    cache.backend().putL1(key, reconstructed);
                    cache.logEvent(id, key, "L2_HIT");
                    out.handles.at(id)->linkTo(reconstructed);
                    depKeys[id] = key;
                    out.keys[id] = key;
                    continue;
                }
            }
            cache.stats().l2_misses++;

            // --- Full bootstrap (cache miss) ---
            auto tBootStart = std::chrono::steady_clock::now();
            auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexReg, curveBump);
            auto tBootEnd = std::chrono::steady_clock::now();
            double bootMs = std::chrono::duration<double, std::milli>(tBootEnd - tBootStart).count();

            cache.stats().bootstraps++;

            // Freeze/serialize ONLY when the serializer can extract the
            // curve's actual bootstrap pillars (a PiecewiseYieldCurve built by
            // TermStructureParser). Otherwise — e.g. a ZeroRatePoint curve,
            // which is an InterpolatedZeroCurve — serialize() would fall back
            // to weekly resampling, and an L1/L2 hit would serve an
            // approximation of the user's nodes instead of the real curve
            // Such curves cache the LIVE curve (value-exact, same
            // as the non-Discount-trait branch) and skip the L2 store.
            bool exactPillars = CurveSerializer::canSerializeExactly(curve, ts);

            std::shared_ptr<QuantLib::YieldTermStructure> toCacheL1 = curve;
            if (exactPillars) {
                // Serialize once; reused for both the L2 store and (for
                // Discount curves) the frozen L1 reconstruction below.
                auto serialized = CurveSerializer::serialize(curve, ts);

                // Cache a frozen, dependency-free curve in L1 for subsequent
                // hits. A live PiecewiseYieldCurve observes its helpers, which
                // observe the discount-curve handle; relinking that handle on
                // a later request (even to identical data) marks the cached
                // curve dirty and forces a full re-bootstrap on next use. The
                // reconstructed InterpolatedDiscountCurve holds only pillar
                // dates and discount factors — it observes nothing, so it
                // never re-bootstraps.
                // Only Discount-trait curves reconstruct bit-exactly; other
                // traits fall back to caching the live curve (today's
                // behavior).
                if (ts->bootstrap_trait() == quantra::enums::BootstrapTrait_Discount) {
                    try {
                        toCacheL1 = CurveSerializer::reconstruct(serialized);
                    } catch (const std::exception&) {
                        // reconstruct() fails closed on data it cannot rebuild
                        // exactly; cache the live curve instead (pre-freeze
                        // behavior) rather than failing the request.
                        toCacheL1 = curve;
                    }
                }

                // Store in L2 (serialized DFs — for future Redis). Gated on
                // exactPillars for the same reason as the freeze: resampled
                // data must never be reconstructed and served on an L2 hit.
                cache.backend().putL2(key, serialized);
            }
            cache.backend().putL1(key, toCacheL1);

            cache.logEvent(id, key, "MISS_BOOTSTRAP", bootMs);

            // The miss request itself stays on the live curve, so its result is
            // byte-identical to the no-cache path; only later L1 hits use the
            // frozen reconstruction.
            out.handles.at(id)->linkTo(curve);
            depKeys[id] = key;
            out.keys[id] = key;
        } else {
            // Cache disabled — original behavior
            auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexReg, curveBump);
            out.handles.at(id)->linkTo(curve);
        }
    }

    return out;
}

} // namespace quantra
