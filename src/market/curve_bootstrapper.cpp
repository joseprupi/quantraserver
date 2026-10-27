#include "curve_bootstrapper.h"

#include "curve_cache.h"
#include "curve_cache_key.h"
#include "curve_serializer.h"
#include "date_convert.h"
#include "request_validation.h"
#include "term_structure_parser.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <queue>

namespace quantra {

// =============================================================================
// Helpers
// =============================================================================

static void addDep(
    std::unordered_map<std::string, std::vector<std::string>>& deps,
    const std::string& from,
    const std::string& to) {
    if (to.empty() || from == to) return;
    deps[from].push_back(to);
}

static void addDepFromRef(
    std::unordered_map<std::string, std::vector<std::string>>& deps,
    const std::string& curveId,
    const quantra::CurveRef* ref) {
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
    const quantra::HelperDependencies* hd) {
    if (!hd) return;
    addDepFromRef(deps, curveId, hd->discount_curve());
}

/// Curves interpolated from explicit points (no rate helpers) observe
/// nothing, so the curve cache can hold them as built.
static bool interpolatesExplicitPoints(const quantra::TermStructure* ts) {
    if (!ts->bootstrap_trait().has_value()) return false;
    switch (ts->bootstrap_trait().value()) {
        case quantra::enums::BootstrapTrait_InterpolatedDiscount:
        case quantra::enums::BootstrapTrait_InterpolatedZero:
        case quantra::enums::BootstrapTrait_InterpolatedFwd: return true;
        default: return false;
    }
}

/// Add all dependency edges (discount + projection).
/// Used for helpers that genuinely encode multi-curve projection relationships
/// (TenorBasisSwapHelper).
static void addAllDeps(
    std::unordered_map<std::string, std::vector<std::string>>& deps,
    const std::string& curveId,
    const quantra::HelperDependencies* hd) {
    if (!hd) return;
    addDepFromRef(deps, curveId, hd->discount_curve());
    addDepFromRef(deps, curveId, hd->projection_curve());
}

// =============================================================================
// Collect dependencies from all helpers in a curve
// =============================================================================

void CurveBootstrapper::collectDeps(
    const quantra::TermStructure* ts,
    std::unordered_map<std::string, std::vector<std::string>>& deps) {
    if (!ts->id()) return;
    std::string curveId = ts->id()->str();

    deps.try_emplace(curveId, std::vector<std::string>{});

    if (!ts->points()) return;

    for (flatbuffers::uoffset_t i = 0; i < ts->points()->size(); i++) {
        auto wrapper = ts->points()->Get(i);
        auto ptype = wrapper->point_type();

        // A union whose type byte is set but whose value offset is absent
        // passes FlatBuffers verification, then the static_casts below would
        // dereference a null table. Reject it as a bad request instead.
        if (ptype != quantra::Point_NONE && wrapper->point() == nullptr) {
            QUANTRA_INVALID_ARGUMENT("Curve point union type is set but its value is missing");
        }

        // Swap/OIS helpers: discount-curve only.
        // QuantLib's SwapRateHelper/OISRateHelper override projection via
        // index->clone(termStructureHandle_), so projection_curve deps are
        // meaningless and would create bogus ordering constraints.
        if (ptype == quantra::Point_SwapHelper) {
            auto h = static_cast<const quantra::SwapHelper*>(wrapper->point());
            addDiscountOnlyDep(deps, curveId, h->deps());
        } else if (ptype == quantra::Point_OISHelper) {
            auto h = static_cast<const quantra::OISHelper*>(wrapper->point());
            addDiscountOnlyDep(deps, curveId, h->deps());
        } else if (ptype == quantra::Point_DatedOISHelper) {
            auto h = static_cast<const quantra::DatedOISHelper*>(wrapper->point());
            addDiscountOnlyDep(deps, curveId, h->deps());
        }
        // Basis helpers: full deps including projection curves.
        // These helpers genuinely use exogenous projection relationships.
        else if (ptype == quantra::Point_TenorBasisSwapHelper) {
            auto h = static_cast<const quantra::TenorBasisSwapHelper*>(wrapper->point());
            addAllDeps(deps, curveId, h->deps());
        }
        // FX helpers: the exogenous curves (the collateral currency's discount
        // curve; for the basis helper also both forwarding curves) are
        // explicit references on the helper, so the curve built from FX
        // pillars always comes after them. An unknown reference is a 404
        // (dependency not provided); a reference back to the curve itself is
        // a 400.
        else if (ptype == quantra::Point_FxSwapHelper) {
            auto h = static_cast<const quantra::FxSwapHelper*>(wrapper->point());
            // The collateral curve is by construction a different curve (the
            // helper builds the OTHER currency's discount curve). Naming the
            // curve under construction would hand QuantLib an empty handle at
            // bootstrap time; reject it here as a plain request error.
            if (h->collateral_curve() && h->collateral_curve()->id() &&
                h->collateral_curve()->id()->str() == curveId) {
                QUANTRA_INVALID_ARGUMENT(
                    "FxSwapHelper.collateral_curve '" + curveId +
                    "' is the curve being bootstrapped; it must name the discount curve of "
                    "the collateral currency (another curve)");
            }
            addDepFromRef(deps, curveId, h->collateral_curve());
        } else if (ptype == quantra::Point_CrossCcyBasisHelper) {
            auto h = static_cast<const quantra::CrossCcyBasisHelper*>(wrapper->point());
            // Exactly one discount curve is given (the collateral side); the
            // absent one is the curve being bootstrapped. Both forwarding
            // curves are exogenous: QuantLib builds the two floating legs
            // from the indices as given, so a leg projected off the curve
            // under construction would read an empty handle at bootstrap.
            auto hasRef = [](const quantra::CurveRef* ref) {
                return ref && ref->id() && !ref->id()->str().empty();
            };
            if (hasRef(h->base_discount_curve()) == hasRef(h->quote_discount_curve())) {
                QUANTRA_INVALID_ARGUMENT(
                    "CrossCcyBasisHelper requires exactly one of base_discount_curve or "
                    "quote_discount_curve: the given one is the collateral currency's discount "
                    "curve, the absent one is the curve being bootstrapped");
            }
            auto rejectSelf = [&](const quantra::CurveRef* ref, const char* field) {
                if (hasRef(ref) && ref->id()->str() == curveId) {
                    QUANTRA_INVALID_ARGUMENT(
                        std::string("CrossCcyBasisHelper.") + field + " '" + curveId +
                        "' is the curve being bootstrapped; it must name another curve of "
                        "pricing.rates.curves");
                }
            };
            rejectSelf(h->base_discount_curve(), "base_discount_curve");
            rejectSelf(h->quote_discount_curve(), "quote_discount_curve");
            rejectSelf(h->base_forwarding_curve(), "base_forwarding_curve");
            rejectSelf(h->quote_forwarding_curve(), "quote_forwarding_curve");
            addDepFromRef(deps, curveId, h->base_discount_curve());
            addDepFromRef(deps, curveId, h->quote_discount_curve());
            addDepFromRef(deps, curveId, h->base_forwarding_curve());
            addDepFromRef(deps, curveId, h->quote_forwarding_curve());
        }
    }
}

// =============================================================================
// Topological sort (Kahn's algorithm)
// =============================================================================

std::vector<std::string> CurveBootstrapper::topoSort(
    const std::unordered_map<std::string, std::vector<std::string>>& deps) {
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
    const RequestBudget& budget,
    const quantra::FxMarketData* fx) const {
    if (!curves || curves->size() == 0) {
        QUANTRA_INVALID_ARGUMENT("curves is required (at least one curve)");
    }

    // ---- 1. Build QuoteRegistry ----
    QuoteRegistry quoteReg;
    if (quotes) {
        for (flatbuffers::uoffset_t i = 0; i < quotes->size(); i++) {
            auto q = quotes->Get(i);
            if (!q->id()) QUANTRA_INVALID_ARGUMENT("QuoteSpec.id is required");
            double v = requireFinite(q->value(), "QuoteSpec.value");
            if (q->quote_type() == quantra::QuoteType_Curve) {
                v += curveBump;
            }
            quoteReg.upsert(q->id()->str(), v, q->quote_type());
        }
    }

    // ---- 2. Build IndexRegistry ----
    IndexRegistryBuilder indexBuilder;
    IndexRegistry indexReg = indexBuilder.build(indices);

    // ---- 2b. Build FxSpotRegistry (FX spots referenced by FxSwapHelper) ----
    // Resolved against THIS quote registry so a spot given by quote_id
    // observes the same quote the pillars do. Fx-typed quotes never receive
    // the curve bump above, so a spot is identical under every rebump leg.
    FxSpotRegistryBuilder fxBuilder;
    FxSpotRegistry fxReg = fxBuilder.build(fx, quoteReg);

    // ---- 3. Index curves by id ----
    std::unordered_map<std::string, const quantra::TermStructure*> curveIndex;
    for (flatbuffers::uoffset_t i = 0; i < curves->size(); i++) {
        auto ts = curves->Get(i);
        if (!ts->id())
            QUANTRA_INVALID_ARGUMENT("TermStructure.id is required for multi-curve bootstrapping");
        std::string curveId = ts->id()->str();
        if (curveIndex.count(curveId) != 0) {
            QUANTRA_INVALID_ARGUMENT("duplicate curve id: " + curveId);
        }
        curveIndex[curveId] = ts;
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
        keyCtx = KeyContext::build(quotes, indices, fx);
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
                    key = CurveKeyBuilder::compute(asOfDate, ts, keyCtx, relevantDepKeys);
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
                auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexReg, curveBump, &fxReg);
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
            auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexReg, curveBump, &fxReg);
            auto tBootEnd = std::chrono::steady_clock::now();
            double bootMs =
                std::chrono::duration<double, std::milli>(tBootEnd - tBootStart).count();

            cache.stats().bootstraps++;

            // L1 holds only curves that observe nothing. A curve bootstrapped
            // from rate helpers observes them, and QuantLib's tenor helpers
            // re-date themselves on every evaluation-date change, with the
            // calendars in force at that moment: other requests would re-date
            // and re-bootstrap a cached one. It is cached as its helper-free
            // copy, which returns exactly the same values, or not at all when
            // no exact copy can be made (including when it fails to bootstrap:
            // the request then meets the error where it uses the curve, as
            // without the cache). A curve interpolated from explicit points
            // observes nothing and is cached as built.
            std::shared_ptr<QuantLib::YieldTermStructure> toCacheL1 =
                CurveSerializer::freeze(curve);
            if (toCacheL1) {
                // L2 (serialized DFs, for a future Redis backend) only when the
                // serializer can extract the curve's actual pillars: resampled
                // data must never be reconstructed and served on an L2 hit.
                if (CurveSerializer::canSerializeExactly(curve, ts)) {
                    cache.backend().putL2(key, CurveSerializer::serialize(curve, ts));
                }
            } else if (interpolatesExplicitPoints(ts)) {
                toCacheL1 = curve;
            }

            if (toCacheL1) {
                cache.backend().putL1(key, toCacheL1);
                cache.logEvent(id, key, "MISS_BOOTSTRAP", bootMs);
                // This request prices on the cached object too, as a hit does;
                // the bootstrapped curve and its helpers are released here.
                curve = toCacheL1;
            } else {
                cache.logEvent(id, key, "MISS_UNCACHED", bootMs);
            }
            out.handles.at(id)->linkTo(curve);
            depKeys[id] = key;
            out.keys[id] = key;
        } else {
            // Cache disabled — original behavior
            auto curve = tsParser.parse(ts, &quoteReg, &curveReg, &indexReg, curveBump, &fxReg);
            out.handles.at(id)->linkTo(curve);
        }
    }

    return out;
}

} // namespace quantra
