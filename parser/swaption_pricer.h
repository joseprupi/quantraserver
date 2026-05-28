#ifndef QUANTRA_SWAPTION_PRICER_H
#define QUANTRA_SWAPTION_PRICER_H

/**
 * SwaptionPricer — pure QuantLib pricing core for swaptions.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * swaption_mapper.{h,cpp}. The pricer reads market data exclusively from the
 * plain-domain registry fields (reg.volatility.modelDomains,
 * reg.volatility.swaptionVols, reg.rates.swapIndices, reg.rates.curves); it
 * never touches reg.volatility.models (legacy raw-FB-pointer field).
 *
 * Rebump greeks need to re-bootstrap curves with a parallel bump and rebuild
 * indices and the QL Swaption against those bumped curves. Both operations
 * are upstream of the pricer in the legacy code path (they consume raw FB
 * tables for curves/quotes/indices/swaption schedule). To keep the pricer
 * FB-free without losing byte-identical rebump semantics, the mapper hands
 * the pricer two opaque callables — `bootstrapWithBump` on the inputs, and
 * `buildSwaption` on each trade — that close over the FB request. The pricer
 * invokes them through the std::function indirection and never sees the
 * underlying request types.
 */

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ql/handle.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/pricingengine.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>

#include "curve_bootstrapper.h"
#include "index_registry.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "vol_surface_parsers.h"

namespace quantra {

/// Closure provided by the mapper that builds a fresh QL Swaption against a
/// caller-supplied IndexRegistry and forwarding-curve handle. Captures the FB
/// swaption table from the request; the pricer never sees it. Reused both
/// for the base valuation and for each rebump step (where the forwarding
/// curve has been bumped or rolled).
using SwaptionInstrumentBuilder = std::function<std::shared_ptr<QuantLib::Swaption>(
    const IndexRegistry& indices,
    const QuantLib::Handle<QuantLib::YieldTermStructure>& forwardingCurve)>;

/// Snapshot of the rebumped market state. Closures capturing the FB request
/// in the mapper produce one of these per rebump step (parallel curve bump or
/// roll). `indices` is the freshly built IndexRegistry — necessary because
/// the IBOR/OIS indices are bound to the (now bumped) forwarding curves.
struct SwaptionRebumpedMarket {
    BootstrappedCurves curves;
    IndexRegistry indices;
};

/// Closure provided by the mapper that re-bootstraps every curve in the
/// request with a parallel `curveBump` (in absolute units) applied to every
/// quote, and rebuilds the IndexRegistry against those bumped curves. The
/// caller is responsible for setting Settings::instance().evaluationDate()
/// to the rebump date *before* invoking this — the bootstrap reads the
/// evaluation date as the implicit reference date for each curve.
using SwaptionRebumpFn = std::function<SwaptionRebumpedMarket(double curveBump)>;

/// Plain SwaptionTrade — everything the pricer needs to value one swaption,
/// pre-parsed by the mapper. Carries:
///   - ID references into the PricingRegistry for curves/vol/model/swap-index
///   - Plain validation context (trade floating-index id, exercise date, swap
///     start date) used by finalizeSwaptionVolEntryForPricing's spot-days
///     and float-index consistency checks
///   - A SwaptionInstrumentBuilder closure that reconstructs the QL Swaption
///     against a possibly-bumped forwarding curve.
struct SwaptionTrade {
    std::string discountingCurveId;
    std::string forwardingCurveId;
    std::string volatilityId;
    std::string modelId;

    /// Float-index id extracted from the underlying swap. Empty for OIS-swap
    /// underlyings (matches legacy SwaptionModelParser behavior).
    std::string tradeFloatIndexId;
    /// First (or only) exercise date — used as the option expiry for vol
    /// surface diagnostics and the spot-days vs swap-start sanity check.
    QuantLib::Date exerciseDate;
    /// Effective date of the underlying swap's fixed leg — used by the
    /// spot-days consistency check inside finalizeSwaptionVolEntryForPricing.
    QuantLib::Date underlyingStartDate;
    /// True when the underlying is a VanillaSwap (false for OIS). Used to
    /// reject the OIS branch in HW Calibrate mode, matching legacy.
    bool underlyingIsVanillaSwap = true;

    SwaptionInstrumentBuilder buildSwaption;
};

struct SwaptionInputs {
    std::vector<SwaptionTrade> trades;
    bool includeDiagnostics = false;
    /// Re-bootstrap-and-reindex closure. Required when rebump greeks are
    /// active (reg.options.swaptionPricingRebump); unused otherwise.
    SwaptionRebumpFn bootstrapWithBump;
};

/// Per-trade pricing result. Mirrors the SwaptionResponse FB schema exactly so
/// the mapper just copies fields. `usedStrikeKind`, `volKind`, and
/// `usedModelParamMode` carry the FB enum values directly (the plain-domain
/// representation reuses the FB enum types via vol_surface_parsers.h and
/// enums.h, neither of which is a *_generated.h header).
struct SwaptionPerTrade {
    double npv = 0.0;
    double impliedVolatility = -1.0;
    double atmForward = 0.0;
    double annuity = 0.0;
    double delta = 0.0;
    double vega = 0.0;
    double gamma = 0.0;
    double theta = 0.0;
    double dv01 = 0.0;
    double usedVolatility = 0.0;
    std::string usedOptionExpiry;
    std::string usedSwapTenor;
    double usedStrike = 0.0;
    double usedAtmForward = -1.0;
    quantra::enums::SwaptionStrikeKind usedStrikeKind =
        quantra::enums::SwaptionStrikeKind_Absolute;
    double usedSpreadFromAtm = 0.0;
    double usedCubeNodeAtm = -1.0;
    quantra::enums::SwaptionVolKind volKind =
        quantra::enums::SwaptionVolKind_Constant;
    quantra::enums::ModelParamMode usedModelParamMode =
        quantra::enums::ModelParamMode_Explicit;
    double usedHwA = -1.0;
    double usedHwSigma = -1.0;
    double usedHwRmse = -1.0;
    int usedHwNumHelpers = -1;
    int usedHwGridRows = -1;
    int usedHwGridCols = -1;
    int usedHwGridPoints = -1;
};

/// SABR-kind vol surfaces emit a SwaptionVolDiagnostics block when the request
/// asks for diagnostics. The pricer finalizes each unique SABR vol surface at
/// most once per request and stores the finalized entry here in first-encounter
/// order so the mapper can emit one diagnostics offset per surface, in the
/// same order as the legacy path.
struct SwaptionResult {
    std::vector<SwaptionPerTrade> values;
    bool includeDiagnostics = false;
    std::vector<std::string> sabrVolIdsInOrder;
    std::map<std::string, SwaptionVolEntry> finalizedSabrEntries;
};

class SwaptionPricer {
public:
    SwaptionResult price(const SwaptionInputs& inputs,
                         const PricingRegistry& reg,
                         const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_SWAPTION_PRICER_H
