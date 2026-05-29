#ifndef QUANTRA_SAMPLE_VOL_SURFACES_PRICER_H
#define QUANTRA_SAMPLE_VOL_SURFACES_PRICER_H

/**
 * SampleVolSurfacesPricer — pure QuantLib core for the volatility-surface
 * sampling endpoint.
 *
 * For each query it resolves the referenced surface in the right plain
 * registry map (optionlet / swaption / black), finalizes SABR / SpreadFromATM
 * swaption surfaces through the shared swaption-vol runtime
 * (finalizeSwaptionVolEntryForPricing / computeServerAtmForwardsForExerciseDates),
 * builds the requested date/tenor/strike grids, and samples the surface at
 * every grid node. Each query is independently validated; failures are
 * captured per-surface so a bad query does not abort the whole response,
 * matching the legacy handler. SABR diagnostics are emitted as plain finalized
 * entries for the mapper to serialize.
 *
 * Grid construction is registry-dependent (the fallback calendar and reference
 * date come from the resolved vol entry / swap index), so it runs here on the
 * plain grid-spec mirrors the mapper lifted out of the FlatBuffers request,
 * rather than in the mapper.
 *
 * INVARIANT: this file (and its .cpp) must NEVER include any *_generated.h or
 * mention FlatBuffers/gRPC namespaces. Suite 0 (scripts/check_pricer_boundary.sh)
 * enforces this with a literal grep. All wire-format conversion lives in
 * sample_vol_surfaces_mapper.{h,cpp}. The pricer reads market data only through
 * the plain registry fields reg.volatility.{swaptionVols,optionletVols,blackVols},
 * reg.rates.{curves,swapIndices,indices}; it never touches reg.volatility.models.
 */

#include <cstdint>
#include <string>
#include <vector>

#include <ql/time/date.hpp>
#include <ql/time/period.hpp>

#include "enums.h"
#include "pricing_context.h"
#include "pricing_registry.h"
#include "vol_surface_parsers.h"

namespace quantra {

// Plain mirror enums (BootstrapCurves-style, scoped to this product). Integer
// values mirror the FlatBuffers vol_query enums so the mapper translates 1:1.
enum class SampleSurfaceType : std::int8_t { Swaption = 0, Optionlet = 1, EquityBlack = 2 };
enum class SampleStrikeAxis : std::int8_t { AbsoluteStrike = 0, SpreadFromATM = 1 };
enum class SampleOutputMode : std::int8_t { Cube = 0, SmileSlice = 1, TermSlice = 2, ExpirySlice = 3 };
enum class SampleExpiryKind : std::int8_t { ExerciseDate = 0, GridDate = 1 };
enum class SampleDateGridKind : std::int8_t { None = 0, Tenor = 1, Range = 2 };

/// Plain mirror of one quantra::Period grid entry. The unit is kept as the
/// wire enum so the pricer converts to QuantLib at the exact point the legacy
/// did (matching the per-query error attribution).
struct SampleTenor {
    int n = 0;
    quantra::enums::TimeUnit unit = quantra::enums::TimeUnit_Days;
};

struct SampleTenorGrid {
    bool hasTenors = false;
    std::vector<SampleTenor> tenors;
    quantra::enums::Calendar calendar = quantra::enums::Calendar_NullCalendar;
    quantra::enums::BusinessDayConvention businessDayConvention =
        quantra::enums::BusinessDayConvention_Following;
};

struct SampleRangeGrid {
    bool hasStartDate = false;
    std::string startDate;
    bool hasEndDate = false;
    std::string endDate;
    int stepNumber = 1;
    quantra::enums::TimeUnit stepTimeUnit = quantra::enums::TimeUnit_Days;
    bool businessDaysOnly = false;
    quantra::enums::Calendar calendar = quantra::enums::Calendar_NullCalendar;
    quantra::enums::BusinessDayConvention businessDayConvention =
        quantra::enums::BusinessDayConvention_Following;
};

struct SampleDateGridSpec {
    bool present = false;
    SampleDateGridKind kind = SampleDateGridKind::None;
    SampleTenorGrid tenor;
    SampleRangeGrid range;
};

struct SampleQueryOptions {
    bool present = false;
    quantra::enums::Calendar calendar = quantra::enums::Calendar_NullCalendar;
    quantra::enums::BusinessDayConvention businessDayConvention =
        quantra::enums::BusinessDayConvention_Following;
    int maxPoints = 50000;
    bool allowExtrapolation = true;
    bool strict = true;
};

/// One vol-surface query, lifted out of the FlatBuffers request by the mapper.
struct SampleVolSurfacesQuery {
    std::string volId;
    SampleSurfaceType surfaceType = SampleSurfaceType::Swaption;

    bool hasStrikeGrid = false;
    bool hasStrikes = false;
    SampleStrikeAxis strikeAxis = SampleStrikeAxis::AbsoluteStrike;
    std::vector<double> strikes;

    SampleDateGridSpec expiryGrid;
    SampleDateGridSpec tenorGrid;
    SampleQueryOptions options;
    SampleOutputMode outputMode = SampleOutputMode::Cube;

    bool hasSwapIndexId = false;
    std::string swapIndexId;

    int sliceExpiryIndex = -1;
    int sliceTenorIndex = -1;
    double sliceStrike = 0.0;
    bool sliceStrikeIsSet = false;

    bool hasDiscountingCurveId = false;
    std::string discountingCurveId;
    bool hasForwardingCurveId = false;
    std::string forwardingCurveId;
};

struct SampleVolSurfacesInputs {
    std::vector<SampleVolSurfacesQuery> queries;
    bool includeDiagnostics = false;
};

/// Per-surface sampling result mirroring the VolSurfaceSample FB table. When
/// hasError is true only volId + error are populated (the mapper emits just
/// vol_id + Error), matching the legacy per-query error entry.
struct VolSurfaceSampleResult {
    std::string volId;
    bool hasError = false;
    std::string error;

    QuantLib::Date referenceDate;
    quantra::enums::VolatilityType qlVolType = quantra::enums::VolatilityType_Lognormal;
    SampleStrikeAxis requestedStrikeAxis = SampleStrikeAxis::AbsoluteStrike;
    quantra::enums::SwaptionStrikeKind canonicalStrikeKind =
        quantra::enums::SwaptionStrikeKind_Absolute;
    bool allowExtrapolationUsed = true;
    quantra::enums::Calendar calendarUsed = quantra::enums::Calendar_NullCalendar;
    quantra::enums::BusinessDayConvention businessDayConventionUsed =
        quantra::enums::BusinessDayConvention_Following;
    SampleExpiryKind expiryKind = SampleExpiryKind::GridDate;

    std::vector<QuantLib::Date> expiries;
    std::vector<QuantLib::Date> requestedExpiryGridPoints;
    std::vector<QuantLib::Period> tenors;
    std::vector<QuantLib::Date> effectiveSwapStarts;
    std::vector<QuantLib::Date> effectiveSwapEnds;
    std::vector<double> strikes;
    std::vector<double> vols;
    std::vector<double> atmLevels;
    int nExpiries = 0;
    int nTenors = 0;
    int nStrikes = 0;
};

/// SABR-kind surface diagnostics. When !partial the mapper serializes the
/// finalized entry via buildSwaptionVolDiagnostics; otherwise it emits a
/// partial block from kind + warnings.
struct SampleVolDiagnostic {
    std::string volId;
    bool partial = false;
    SwaptionVolEntry finalized;
    quantra::enums::SwaptionVolKind kind = quantra::enums::SwaptionVolKind_Constant;
    std::vector<std::string> warnings;
};

struct SampleVolSurfacesResult {
    std::vector<VolSurfaceSampleResult> samples;
    std::vector<SampleVolDiagnostic> diagnostics;
};

class SampleVolSurfacesPricer {
public:
    SampleVolSurfacesResult price(const SampleVolSurfacesInputs& inputs,
                                  const PricingRegistry& reg,
                                  const PricingContext& ctx) const;
};

} // namespace quantra

#endif // QUANTRA_SAMPLE_VOL_SURFACES_PRICER_H
