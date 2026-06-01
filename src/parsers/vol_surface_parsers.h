#ifndef QUANTRA_VOL_SURFACE_PARSERS_H
#define QUANTRA_VOL_SURFACE_PARSERS_H

/**
 * Vol Surface Parsers
 * 
 * Parsers for typed volatility surfaces. Each vol type has its own parser.
 * 
 * Supported types:
 *   - OptionletVolSpec  -> ConstantOptionletVolatility (caps/floors)
 *   - SwaptionVolSpec   -> ConstantSwaptionVolatility (swaptions)
 *   - BlackVolSpec      -> BlackConstantVol (equity/FX options)
 */

#include <ql/handle.hpp>
#include <ql/termstructures/volatility/optionlet/optionletvolatilitystructure.hpp>
#include <ql/termstructures/volatility/swaption/swaptionvolstructure.hpp>
#include <ql/termstructures/volatility/equityfx/blackvoltermstructure.hpp>
#include <ql/termstructures/volatility/optionlet/constantoptionletvol.hpp>
#include <ql/termstructures/volatility/swaption/swaptionconstantvol.hpp>
#include <vector>
#include <string>
#include <map>
#include <ql/termstructures/volatility/equityfx/blackconstantvol.hpp>
#include <ql/time/date.hpp>
#include <ql/time/calendar.hpp>
#include <ql/time/daycounter.hpp>
#include <ql/indexes/swapindex.hpp>

#include "volatility_generated.h"
#include "enum_convert.h"
#include "date_convert.h"
#include "quote_registry.h"

namespace quantra {

// =============================================================================
// Vol Entry structs - store handle + metadata for engine factory validation
// =============================================================================

struct OptionletVolEntry {
    QuantLib::Handle<QuantLib::OptionletVolatilityStructure> handle;
    QuantLib::VolatilityType qlVolType;
    double displacement;
    double constantVol;
    QuantLib::Date referenceDate;
    QuantLib::Calendar calendar;
    quantra::enums::Calendar calendarFb = quantra::enums::Calendar_NullCalendar;
    QuantLib::BusinessDayConvention businessDayConvention = QuantLib::Following;
    quantra::enums::BusinessDayConvention businessDayConventionFb =
        quantra::enums::BusinessDayConvention_Following;
    QuantLib::DayCounter dayCounter;
};

struct SwaptionVolEntry {
    QuantLib::Handle<QuantLib::SwaptionVolatilityStructure> handle;
    QuantLib::VolatilityType qlVolType;
    double displacement;
    double constantVol;
    QuantLib::Date referenceDate;
    QuantLib::Calendar calendar;
    QuantLib::BusinessDayConvention businessDayConvention;
    QuantLib::DayCounter dayCounter;
    quantra::enums::SwaptionVolKind volKind = quantra::enums::SwaptionVolKind_Constant;
    std::vector<QuantLib::Period> expiries;
    std::vector<QuantLib::Period> tenors;
    std::vector<double> strikes;
    std::string swapIndexId;
    quantra::enums::SwaptionStrikeKind strikeKind = quantra::enums::SwaptionStrikeKind_Absolute;
    bool allowExternalAtm = false;
    std::vector<double> volsFlat;
    std::vector<double> atmForwardsFlat;
    std::vector<double> sabrAlpha;
    std::vector<double> sabrBeta;
    std::vector<double> sabrRho;
    std::vector<double> sabrNu;
    // SABR calibrate-path inputs and diagnostics. Stored separately from
    // volsFlat/strikes (which carry SmileCube/AtmMatrix semantics) to avoid
    // semantic collisions with downstream sampling/bumping helpers — see
    // sample_vol_surfaces_request.cpp strike-support bounds checks.
    // sabrStrikeSpreads: strike spreads from per-node ATM forward, length nStrikes.
    // sabrMarketVolsFlat: row-major nExp*nTen*nStrikes absolute market vols.
    std::vector<double> sabrStrikeSpreads;
    std::vector<double> sabrMarketVolsFlat;
    // Calibration options, populated by the calibrate parser; ignored otherwise.
    bool sabrBetaFixed = true;
    double sabrBetaValue = 0.5;
    bool sabrVegaWeightedSmileFit = false;
    // Per-node calibration diagnostics, populated by withSwaptionSabrCalibrateAtm
    // after the QL cube has run. Empty for SabrParams surfaces.
    // Row-major nExp*nTen.
    std::vector<double> sabrPerNodeRmse;
    std::vector<double> sabrPerNodeMaxError;
    std::vector<int> sabrPerNodeEndCriteria;
    int nExp = 0;
    int nTen = 0;
    int nStrikes = 0;
};

struct BlackVolEntry {
    QuantLib::Handle<QuantLib::BlackVolTermStructure> handle;
    double constantVol;
    QuantLib::Date referenceDate;
    QuantLib::Calendar calendar;
    quantra::enums::Calendar calendarFb = quantra::enums::Calendar_NullCalendar;
    QuantLib::BusinessDayConvention businessDayConvention = QuantLib::Following;
    quantra::enums::BusinessDayConvention businessDayConventionFb =
        quantra::enums::BusinessDayConvention_Following;
    QuantLib::DayCounter dayCounter;
};

// =============================================================================
// Parser functions - free functions (not class methods)
// =============================================================================

/**
 * Convert Quantra enum to QuantLib VolatilityType.
 */
QuantLib::VolatilityType toQlVolType(quantra::enums::VolatilityType t);

/**
 * Parse OptionletVolSpec from FlatBuffers into QuantLib structure.
 */
OptionletVolEntry parseOptionletVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes = nullptr);

/**
 * Parse SwaptionVolSpec from FlatBuffers into QuantLib structure.
 */
SwaptionVolEntry parseSwaptionVol(const quantra::VolSurfaceSpec* spec, const QuoteRegistry* quotes = nullptr);

/**
 * Build a bumped swaption vol entry (parallel bump).
 */
SwaptionVolEntry bumpSwaptionVolEntry(const SwaptionVolEntry& base, double volBump);

/**
 * Rebuild smile cube entry with server-computed ATM forwards.
 */
SwaptionVolEntry withSwaptionSmileCubeAtm(
    const SwaptionVolEntry& base,
    const std::vector<double>& atmForwardsFlat);

/**
 * Build a SABR params swaption vol structure from per-node ATM forwards.
 *
 * `base` must have volKind == SwaptionVolKind_SabrParams with sabrAlpha/Beta/Rho/Nu
 * populated row-major (length nExp * nTen). `atmForwardsFlat` supplies the per-node
 * forward F(expiry, tenor) row-major (length nExp * nTen), used to instantiate one
 * QuantLib::SabrSmileSection per node. Returns an entry with `handle` and
 * `atmForwardsFlat` populated; other fields are passed through unchanged.
 */
SwaptionVolEntry withSwaptionSabrParamsAtm(
    const SwaptionVolEntry& base,
    const std::vector<double>& atmForwardsFlat);

/**
 * Build a SABR-calibrated swaption vol structure from per-node ATM forwards.
 *
 * Calls into QuantLib's XabrSwaptionVolatilityCube<SwaptionVolCubeSabrModel>,
 * which runs per-node Levenberg-Marquardt calibration via SABRInterpolation.
 *
 * `base` must have volKind == SwaptionVolKind_SabrCalibrate with strike spreads
 * in `sabrStrikeSpreads` and market vols in `sabrMarketVolsFlat`.
 *
 * Returns an entry with `handle` set, `atmForwardsFlat` populated, calibrated
 * (alpha, beta, rho, nu) per-node grids in sabrAlpha/Beta/Rho/Nu, and per-node
 * fit diagnostics in sabrPerNodeRmse/sabrPerNodeMaxError/sabrPerNodeEndCriteria.
 *
 * `swapIndexBase` and `cubeCacheKey` are emitted by the caller after
 * forward-resolution and curve-key resolution; the cache lookup happens before
 * invoking the QuantLib calibrator. Pass empty `cubeCacheKey` to bypass caching
 * (used by sanity tests).
 */
SwaptionVolEntry withSwaptionSabrCalibrateAtm(
    const SwaptionVolEntry& base,
    const std::vector<double>& atmForwardsFlat,
    const std::shared_ptr<QuantLib::SwapIndex>& swapIndexBase,
    const std::string& cubeCacheKey);

/**
 * Parse BlackVolSpec from FlatBuffers into QuantLib structure.
 */
BlackVolEntry parseBlackVol(
    const quantra::VolSurfaceSpec* spec,
    const QuoteRegistry* quotes = nullptr,
    const std::map<std::string, std::shared_ptr<QuantLib::RelinkableHandle<QuantLib::YieldTermStructure>>>* curves =
        nullptr);

} // namespace quantra

#endif // QUANTRA_VOL_SURFACE_PARSERS_H
