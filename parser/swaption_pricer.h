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
 * reg.volatility.swaptionVols, reg.rates.swapIndices, reg.rates.curves).
 *
 * Rebump greeks need to re-bootstrap curves with a parallel bump and rebuild
 * indices and the QL Swaption against those bumped curves. The curve/index
 * re-bootstrap is upstream of the pricer in the legacy code path (it consumes
 * raw FB tables for curves/quotes/indices). To keep the pricer FB-free without
 * losing byte-identical rebump semantics, the mapper EAGERLY pre-builds the
 * small fixed set of bumped/rolled market snapshots (SwaptionRebumpMarkets) and
 * hands them to the pricer as plain data on the inputs — no callable, no FB
 * indirection. The QL Swaption itself is rebuilt from the plain-domain
 * SwaptionInstrument carried on each trade, so the pricer reconstructs the
 * swaption against the selected (possibly bumped) forwarding curve and
 * IndexRegistry directly.
 */

#include <map>
#include <memory>
#include <string>
#include <vector>

#include <ql/handle.hpp>
#include <ql/instruments/swaption.hpp>
#include <ql/pricingengine.hpp>
#include <ql/termstructures/yieldtermstructure.hpp>
#include <ql/time/date.hpp>

#include <ql/instruments/swaption.hpp>

#include "curve_bootstrapper.h"
#include "enums.h"
#include "index_registry.h"
#include "ois_swap_pricer.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "swaption_rebump.h"
#include "vanilla_swap_pricer.h"
#include "vol_surface_parsers.h"

namespace quantra {

/// Plain-domain swaption instrument — everything SwaptionParser.parse reads,
/// lifted out of the FB request by the mapper. The pricer reconstructs the
/// QuantLib::Swaption from this against a (possibly bumped) forwarding-curve
/// handle and IndexRegistry, reproducing the legacy parse() byte-for-byte. The
/// eval-date-dependent pieces (American exercise reference date, Bermudan
/// date validation) are evaluated at construction time, so the same instrument
/// rebuilds correctly under the rolled evaluation date of the theta leg.
struct SwaptionInstrument {
    quantra::enums::ExerciseType exerciseType =
        quantra::enums::ExerciseType_European;
    /// True when the FB swaption carried a single exercise_date. Mirrors the
    /// legacy `exercise_date() != NULL` requiredness check for European/American.
    bool hasExerciseDate = false;
    /// Single exercise date (European/American). Default Date() when absent.
    QuantLib::Date exerciseDate;
    /// Bermudan exercise date set. Empty when the FB field was absent.
    std::vector<QuantLib::Date> bermudanExerciseDates;
    quantra::enums::SettlementType settlementType =
        quantra::enums::SettlementType_Physical;
    QuantLib::Settlement::Method settlementMethod = QuantLib::Settlement::PhysicalOTC;

    /// Underlying swap. Exactly one branch is populated, selected by
    /// underlyingIsVanilla. The vanilla branch is always the IBOR leg —
    /// SwaptionParser only ever built the IBOR underlying.
    bool underlyingIsVanilla = true;
    VanillaSwapTrade vanillaUnderlying;
    OisSwapTrade oisUnderlying;
};

/// Snapshot of one rebumped market state. The mapper eagerly bootstraps one of
/// these per distinct rebump scenario (parallel curve bump up/down or eval-date
/// roll). `indices` is the freshly built IndexRegistry — necessary because the
/// IBOR/OIS indices are bound to the (now bumped) forwarding curves. Every
/// snapshot's curves are bootstrapped at the SAME evaluation date the pricer
/// later sets when pricing against them, so the rebump greeks are byte-identical
/// to the legacy re-bootstrap-on-demand path.
struct SwaptionRebumpedMarket {
    BootstrappedCurves curves;
    IndexRegistry indices;
};

/// The fixed set of pre-built market snapshots the rebump greeks need. The
/// mapper populates these (and sets `present`) only when the request asks for
/// rebump greeks; otherwise they are left default and unused. The two vol-bump
/// legs (curveBump==0, rollDays==0) deliberately have no snapshot here — they
/// price against the base registry curves at asOf, which are already bootstrapped
/// identically.
///   - curveUp:   parallel +curveBump @ asOf            (DV01/gamma up leg)
///   - curveDown: parallel -curveBump @ asOf            (DV01/gamma down leg)
///   - roll:      bump 0 @ asOf + rollDays              (theta leg)
struct SwaptionRebumpMarkets {
    SwaptionRebumpedMarket curveUp;
    SwaptionRebumpedMarket curveDown;
    SwaptionRebumpedMarket roll;
    bool present = false;
};

/// Plain SwaptionTrade — everything the pricer needs to value one swaption,
/// pre-parsed by the mapper. Carries:
///   - ID references into the PricingRegistry for curves/vol/model/swap-index
///   - Plain validation context (trade floating-index id, exercise date, swap
///     start date) used by finalizeSwaptionVolEntryForPricing's spot-days
///     and float-index consistency checks
///   - A plain SwaptionInstrument from which the pricer reconstructs the QL
///     Swaption against a possibly-bumped forwarding curve.
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

    SwaptionInstrument instrument;
};

struct SwaptionInputs {
    std::vector<SwaptionTrade> trades;
    bool includeDiagnostics = false;
    /// Pre-built bumped/rolled market snapshots. Populated by the mapper only
    /// when rebump greeks are active (reg.options.swaptionPricingRebump); the
    /// `present` flag is false otherwise. Plain data — no callable.
    SwaptionRebumpMarkets rebumpMarkets;
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
